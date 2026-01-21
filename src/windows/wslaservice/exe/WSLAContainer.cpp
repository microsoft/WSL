/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAContainer.cpp

Abstract:

    Contains the implementation of WSLAContainer.

--*/

#include "precomp.h"
#include "WSLAContainer.h"
#include "WSLAProcess.h"
#include "WSLAProcessIO.h"

using wsl::windows::service::wsla::VolumeMountInfo;
using wsl::windows::service::wsla::WSLAContainer;
using wsl::windows::service::wsla::WSLAContainerImpl;
using wsl::windows::service::wsla::WSLAVirtualMachine;

using namespace wsl::windows::common::docker_schema;

namespace {

// TODO: Determine when ports should be mapped and unmapped (at container creation, start, stop or delete).

auto ProcessPortMappings(const WSLA_CONTAINER_OPTIONS& options, WSLAVirtualMachine& vm)
{
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        options.PortsCount > 0 && options.ContainerNetwork.ContainerNetworkType == WSLA_CONTAINER_NETWORK_NONE,
        "Port mappings are not supported without networking");

    // Validate that port mappings are valid.
    // N.B. If a host port is duplicated, MapPort() will fail later.
    for (ULONG i = 0; i < options.PortsCount; i++)
    {
        const auto& port = options.Ports[i];
        THROW_HR_IF_MSG(E_INVALIDARG, port.Family != AF_INET && port.Family != AF_INET6, "Invalid family for port mapping %i: %i", i, port.Family);
    }

    // Generate Windows <-> VM port mappings depending on the networking mode.
    // N.B. pointers are used so the vectors are still available if the errorCleanup is executed.
    auto vmPorts = std::make_shared<std::set<uint16_t>>();
    auto mappedPorts = std::make_shared<std::vector<WSLAContainerImpl::PortMapping>>();

    auto errorCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [mappedPorts = mappedPorts, vmPorts = vmPorts, &vm]() {
        if (!vmPorts->empty())
        {
            LOG_IF_FAILED(wil::ResultFromException([&]() { vm.ReleasePorts(*vmPorts); }));
        }

        for (const auto& e : *mappedPorts)
        {
            if (e.MappedToHost)
            {
                try
                {
                    vm.UnmapPort(e.Family, e.HostPort, e.VmPort);
                }
                CATCH_LOG();
            }
        }
    });

    if (options.ContainerNetwork.ContainerNetworkType == WSLA_CONTAINER_NETWORK_BRIDGE)
    {
        // If the container is in bridged mode, allocate one port in the VM for each port mapping.
        *vmPorts = vm.AllocatePorts(static_cast<uint16_t>(options.PortsCount));

        auto vmPortIt = vmPorts->begin();
        for (ULONG i = 0; i < options.PortsCount; i++)
        {
            WI_ASSERT(vmPortIt != vmPorts->end());

            const auto& port = options.Ports[i];

            mappedPorts->push_back({port.HostPort, *vmPortIt, port.ContainerPort, port.Family});
            vmPortIt++;
        }
    }
    else if (options.ContainerNetwork.ContainerNetworkType == WSLA_CONTAINER_NETWORK_HOST)
    {
        // In host mode, the container port is the same as the VM port.
        for (ULONG i = 0; i < options.PortsCount; i++)
        {
            const auto& port = options.Ports[i];

            // Only allocate a VM port if it hasn't already been allocated to that container.
            // A user can allocate two different host ports to the same container port.
            if (std::ranges::find(*vmPorts, port.ContainerPort) == vmPorts->end())
            {
                THROW_WIN32_IF_MSG(
                    ERROR_ALREADY_EXISTS, !vm.TryAllocatePort(port.ContainerPort), "Failed to allocate port: %u", options.Ports[i].ContainerPort);

                vmPorts->insert(port.ContainerPort);
            }

            mappedPorts->push_back({port.HostPort, port.ContainerPort, port.ContainerPort, port.Family});
        }
    }
    else
    {
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            options.PortsCount > 0,
            "Port mappings are not supported in networking mode: %i",
            options.ContainerNetwork.ContainerNetworkType);
    }

    // Map Windows <-> VM ports.
    for (auto& e : *mappedPorts)
    {
        vm.MapPort(e.Family, e.HostPort, e.VmPort);
        e.MappedToHost = true;
    }

    return std::make_pair(std::move(mappedPorts), std::move(errorCleanup));
}

} // namespace

static constexpr DWORD deleteTimeout = 60000; // 60 seconds

WSLAContainerImpl::WSLAContainerImpl(
    WSLAVirtualMachine* parentVM,
    const WSLA_CONTAINER_OPTIONS& Options,
    std::string&& Id,
    std::vector<VolumeMountInfo>&& volumes,
    std::vector<PortMapping>&& ports,
    std::function<void(const WSLAContainerImpl*)>&& onDeleted,
    ContainerEventTracker& EventTracker,
    DockerHTTPClient& DockerClient) :
    m_parentVM(parentVM),
    m_name(Options.Name == nullptr ? "" : Options.Name), // TODO: get name from docker.
    m_image(Options.Image),
    m_id(std::move(Id)),
    m_mountedVolumes(std::move(volumes)),
    m_mappedPorts(std::move(ports)),
    m_comWrapper(wil::MakeOrThrow<WSLAContainer>(this, std::move(onDeleted))),
    m_dockerClient(DockerClient),
    m_eventTracker(EventTracker),
    m_containerEvents(EventTracker.RegisterContainerStateUpdates(
        m_id, std::bind(&WSLAContainerImpl::OnEvent, this, std::placeholders::_1, std::placeholders::_2)))
{
    m_state = WslaContainerStateCreated;

    // TODO: Move this to an API flag.
    m_tty = ParseFdStatus(Options.InitProcessOptions).second;
}

WSLAContainerImpl::~WSLAContainerImpl()
{
    WSL_LOG(
        "~WSLAContainerImpl",
        TraceLoggingValue(m_name.c_str(), "Name"),
        TraceLoggingValue(m_id.c_str(), "Id"),
        TraceLoggingValue((int)m_state, "State"));

    // Remove container callback from any outstanding processes.
    {
        std::lock_guard lock(m_lock);

        if (m_initProcessControl)
        {
            m_initProcessControl->OnContainerReleased();
        }

        for (auto& process : m_processes)
        {
            process->OnContainerReleased();
        }
    }

    m_containerEvents.Reset();

    // Disconnect from the COM instance. After this returns, no COM calls can be made to this instance.
    m_comWrapper->Disconnect();

    // TODO: Stop and delete running containers when the session is shutting down
    // so that we don't leak resources since we do not have means to track them after
    // restarting a session from a persisted storage.

    if (m_state == WslaContainerStateExited)
    {
        try
        {
            Delete();
        }
        CATCH_LOG();
    }

    // Release port mappings.
    std::set<uint16_t> allocatedGuestPorts;
    for (const auto& e : m_mappedPorts)
    {
        WI_ASSERT(e.MappedToHost);

        try
        {
            m_parentVM->UnmapPort(e.Family, e.HostPort, e.VmPort);
        }
        CATCH_LOG();

        allocatedGuestPorts.insert(e.VmPort);
    }

    m_parentVM->ReleasePorts(allocatedGuestPorts);
}

void WSLAContainerImpl::OnProcessReleased(DockerExecProcessControl* process)
{
    std::lock_guard lock(m_lock);

    auto remove = std::ranges::remove_if(m_processes, [process](const auto* e) { return e == process; });
    WI_ASSERT(remove.size() == 1);

    m_processes.erase(remove.begin(), remove.end());
}

const std::string& WSLAContainerImpl::Image() const noexcept
{
    return m_image;
}

const std::string& WSLAContainerImpl::Name() const noexcept
{
    return m_name;
}

IWSLAContainer& WSLAContainerImpl::ComWrapper()
{
    return *m_comWrapper.Get();
}

void WSLAContainerImpl::Start()
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        m_state != WslaContainerStateCreated,
        "Cannot start container '%hs', state: %i",
        m_name.c_str(),
        m_state);

    // Attach to the container's init process so no IO is lost.
    std::unique_ptr<WSLAProcessIO> io;
    if (m_tty)
    {
        io = std::make_unique<TTYProcessIO>(wil::unique_handle{(HANDLE)m_dockerClient.AttachContainer(m_id).release()});
    }
    else
    {
        io = std::make_unique<RelayedProcessIO>(wil::unique_handle{(HANDLE)m_dockerClient.AttachContainer(m_id).release()});
    }

    auto control = std::make_unique<DockerContainerProcessControl>(*this, m_dockerClient, m_eventTracker);
    m_initProcessControl = control.get();

    m_initProcess = wil::MakeOrThrow<WSLAProcess>(std::move(control), std::move(io));

    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this]() mutable {
        m_initProcess.Reset();
        m_initProcessControl = nullptr;
    });

    m_dockerClient.StartContainer(m_id);

    m_state = WslaContainerStateRunning;
    cleanup.release();
}

void WSLAContainerImpl::OnEvent(ContainerEvent event, std::optional<int> exitCode)
{
    if (event == ContainerEvent::Stop)
    {
        THROW_HR_IF(E_UNEXPECTED, !exitCode.has_value());

        std::lock_guard<std::recursive_mutex> lock(m_lock);
        m_state = WslaContainerStateExited;

        // Notify all processes that the container has exited.
        // N.B. The exec callback isn't always sent to execed processes, so do this to avoid 'stuck' processes.
        for (auto& process : m_processes)
        {
            process->OnContainerReleased();
        }

        m_processes.clear();
    }

    WSL_LOG(
        "ContainerEvent",
        TraceLoggingValue(m_name.c_str(), "Name"),
        TraceLoggingValue(m_id.c_str(), "Id"),
        TraceLoggingValue((int)event, "Event"));
}

void WSLAContainerImpl::Stop(int Signal, ULONG TimeoutMs)
{
    std::lock_guard lock(m_lock);

    if (State() == WslaContainerStateExited)
    {
        return;
    }

    try
    {
        m_dockerClient.StopContainer(m_id, Signal, static_cast<ULONG>(std::round<ULONG>(TimeoutMs / 1000)));
    }
    catch (const DockerHTTPException& e)
    {
        // HTTP 304 is returned when the container is already stopped.
        if (e.StatusCode() == 304)
        {
            return;
        }

        WSL_LOG(
            "StopContainerFailed",
            TraceLoggingValue(m_name.c_str(), "Name"),
            TraceLoggingValue(m_id.c_str(), "Id"),
            TraceLoggingValue(e.what(), "Error"));
        throw;
    }

    m_state = WslaContainerStateExited;
}

void WSLAContainerImpl::Delete()
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    // Validate that the container is in the exited state.
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        State() != WslaContainerStateExited,
        "Cannot delete container '%hs', state: %i",
        m_name.c_str(),
        m_state);

    m_dockerClient.DeleteContainer(m_id);

    UnmountVolumes(m_mountedVolumes, *m_parentVM);

    m_state = WslaContainerStateDeleted;
}

WSLA_CONTAINER_STATE WSLAContainerImpl::State() noexcept
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    if (m_state == WslaContainerStateRunning && m_initProcessControl && m_initProcessControl->GetState().first != WSLAProcessStateRunning)
    {
        m_state = WslaContainerStateExited;
    }

    return m_state;
}

void WSLAContainerImpl::GetState(WSLA_CONTAINER_STATE* Result)
{
    *Result = State();
}

void WSLAContainerImpl::GetInitProcess(IWSLAProcess** Process)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_initProcess);
    THROW_IF_FAILED(m_initProcess.CopyTo(__uuidof(IWSLAProcess), (void**)Process));
}

void WSLAContainerImpl::Exec(const WSLA_PROCESS_OPTIONS* Options, IWSLAProcess** Process, int* Errno)
{
    THROW_HR_IF_MSG(E_INVALIDARG, Options->Executable != nullptr, "Executable must be null");

    std::lock_guard lock{m_lock};

    auto state = State();
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        state != WslaContainerStateRunning,
        "Container %hs is not running. State: %i",
        m_name.c_str(),
        state);

    auto [hasStdin, hasTty] = ParseFdStatus(*Options);

    common::docker_schema::CreateExec request{};
    request.AttachStdout = true;
    request.AttachStderr = true;
    for (ULONG i = 0; i < Options->CommandLineCount; i++)
    {
        request.Cmd.push_back(Options->CommandLine[i]);
    }

    for (ULONG i = 0; i < Options->EnvironmentCount; i++)
    {
        request.Env.push_back(Options->Environment[i]);
    }

    if (Options->CurrentDirectory != nullptr)
    {
        request.WorkingDir = Options->CurrentDirectory;
    }

    if (hasTty)
    {
        request.Tty = true;
    }

    if (hasStdin)
    {
        request.AttachStdin = true;
    }

    try
    {
        auto result = m_dockerClient.CreateExec(m_id, request);

        // N.B. There's no way to delete a created exec instance, it is removed when the container is deleted.

        wil::unique_handle stream{(HANDLE)m_dockerClient
                                      .StartExec(result.Id, common::docker_schema::StartExec{.Tty = hasTty, .ConsoleSize = request.ConsoleSize})
                                      .release()};
        std::unique_ptr<WSLAProcessIO> io;
        if (hasTty)
        {
            io = std::make_unique<TTYProcessIO>(std::move(stream));
        }
        else
        {
            io = std::make_unique<RelayedProcessIO>(std::move(stream));
        }

        auto control = std::make_unique<DockerExecProcessControl>(*this, result.Id, m_dockerClient, m_eventTracker);

        // Store a non owning reference to the process.
        m_processes.push_back(control.get());

        auto process = wil::MakeOrThrow<WSLAProcess>(std::move(control), std::move(io));

        THROW_IF_FAILED(process.CopyTo(__uuidof(IWSLAProcess), (void**)Process));
    }
    catch (DockerHTTPException& e)
    {
        THROW_HR_MSG(E_FAIL, "Failed to exec process in container %hs: %hs", m_id.c_str(), e.what());
    }
}

std::pair<bool, bool> WSLAContainerImpl::ParseFdStatus(const WSLA_PROCESS_OPTIONS& Options)
{
    bool hasStdin = false;
    bool hasTty = false;
    for (size_t i = 0; i < Options.FdsCount; i++)
    {
        if (Options.Fds[i].Fd == 0)
        {
            hasStdin = true;
        }

        if (Options.Fds[i].Type == WSLAFdTypeTerminalInput || Options.Fds[i].Type == WSLAFdTypeTerminalOutput)
        {
            hasTty = true;
        }
    }

    return {hasStdin, hasTty};
}

void WSLAContainerImpl::AddEnvironmentVariables(std::vector<std::string>& args, const WSLA_PROCESS_OPTIONS& options)
{
    for (ULONG i = 0; i < options.EnvironmentCount; i++)
    {
        THROW_HR_IF_MSG(E_INVALIDARG, options.Environment[i][0] == '-', "Invalid environment string: %hs", options.Environment[i]);

        args.insert(args.end(), {"-e", options.Environment[i]});
    }
}

std::vector<VolumeMountInfo> wsl::windows::service::wsla::WSLAContainerImpl::MountVolumes(const WSLA_CONTAINER_OPTIONS& Options, WSLAVirtualMachine& parentVM)
{
    std::vector<VolumeMountInfo> mountedVolumes;
    mountedVolumes.reserve(Options.VolumesCount);

    for (ULONG i = 0; i < Options.VolumesCount; i++)
    {
        try
        {
            const WSLA_VOLUME& volume = Options.Volumes[i];
            std::string parentVMPath = std::format("/mnt/wsla/{}/volumes/{}", Options.Name, i);

            auto result = parentVM.MountWindowsFolder(volume.HostPath, parentVMPath.c_str(), volume.ReadOnly);
            THROW_IF_FAILED_MSG(result, "Failed to mount %ls -> %hs", volume.HostPath, parentVMPath.c_str());

            mountedVolumes.push_back(VolumeMountInfo{volume.HostPath, parentVMPath, volume.ContainerPath, static_cast<bool>(volume.ReadOnly)});
        }
        catch (...)
        {
            // On failure, unmount all previously mounted volumes.
            UnmountVolumes(mountedVolumes, parentVM);
            throw;
        }
    }

    return mountedVolumes;
}

void wsl::windows::service::wsla::WSLAContainerImpl::UnmountVolumes(const std::vector<VolumeMountInfo>& volumes, WSLAVirtualMachine& parentVM)
{
    for (const auto& volume : volumes)
    {
        LOG_IF_FAILED(parentVM.UnmountWindowsFolder(volume.ParentVMPath.c_str()));
    }
}

std::unique_ptr<WSLAContainerImpl> WSLAContainerImpl::Create(
    const WSLA_CONTAINER_OPTIONS& containerOptions,
    WSLAVirtualMachine& parentVM,
    std::function<void(const WSLAContainerImpl*)>&& OnDeleted,
    ContainerEventTracker& EventTracker,
    DockerHTTPClient& DockerClient)
{
    // TODO: Think about when 'StdinOnce' should be set.
    auto [hasStdin, hasTty] = ParseFdStatus(containerOptions.InitProcessOptions);

    common::docker_schema::CreateContainer request;
    request.Image = containerOptions.Image;

    if (hasTty)
    {
        request.Tty = true;
    }

    if (hasStdin)
    {
        request.OpenStdin = true;
        request.StdinOnce = true;
    }

    if (containerOptions.InitProcessOptions.CommandLineCount > 0)
    {
        request.Cmd.insert(
            request.Cmd.end(),
            containerOptions.InitProcessOptions.CommandLine,
            containerOptions.InitProcessOptions.CommandLine + containerOptions.InitProcessOptions.CommandLineCount);
    }

    if (containerOptions.InitProcessOptions.Executable != nullptr)
    {
        request.Entrypoint = std::vector<std::string>{containerOptions.InitProcessOptions.Executable};
    }

    for (DWORD i = 0; i < containerOptions.InitProcessOptions.EnvironmentCount; i++)
    {
        request.Env.push_back(containerOptions.InitProcessOptions.Environment[i]);
    }

    // Mount volumes.
    auto volumes = MountVolumes(containerOptions, parentVM);

    for (const auto& e : volumes)
    {
        request.HostConfig.Mounts.emplace_back(
            common::docker_schema::Mount{.Source = e.ParentVMPath, .Target = e.ContainerPath, .Type = "bind", .ReadOnly = e.ReadOnly});
    }

    // Set the networking mode.
    if (containerOptions.ContainerNetwork.ContainerNetworkType == WSLA_CONTAINER_NETWORK_BRIDGE)
    {
        request.HostConfig.NetworkMode = "bridge";
    }
    else if (containerOptions.ContainerNetwork.ContainerNetworkType == WSLA_CONTAINER_NETWORK_HOST)
    {
        request.HostConfig.NetworkMode = "host";
    }
    else if (containerOptions.ContainerNetwork.ContainerNetworkType == WSLA_CONTAINER_NETWORK_NONE)
    {
        request.HostConfig.NetworkMode = "none";
    }
    else
    {
        THROW_HR_MSG(E_INVALIDARG, "Invalid networking mode: %i", containerOptions.ContainerNetwork.ContainerNetworkType);
    }

    // Process port bindings.
    auto [mappedPorts, errorCleanup] = ProcessPortMappings(containerOptions, parentVM);

    for (const auto& e : *mappedPorts)
    {
        // TODO: UDP support
        // TODO: Investigate ipv6 support.
        auto portKey = std::format("{}/tcp", e.ContainerPort);
        request.ExposedPorts[portKey] = {};
        auto& portEntry = request.HostConfig.PortBindings[portKey];
        portEntry.emplace_back(common::docker_schema::PortMapping{.HostIp = "127.0.0.1", .HostPort = std::to_string(e.VmPort)});
    }

    // Send the request to docker.
    auto result =
        DockerClient.CreateContainer(request, containerOptions.Name != nullptr ? containerOptions.Name : std::optional<std::string>{});

    // N.B. mappedPorts is explicitly copied because it's referenced in errorCleanup, so it can't be moved.
    auto container = std::make_unique<WSLAContainerImpl>(
        &parentVM, containerOptions, std::move(result.Id), std::move(volumes), std::vector<PortMapping>(*mappedPorts), std::move(OnDeleted), EventTracker, DockerClient);

    errorCleanup.release();

    return container;
}

const std::string& WSLAContainerImpl::ID() const noexcept
{
    return m_id;
}

void WSLAContainerImpl::Inspect(LPSTR* Output)
{
    try
    {
        *Output = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(m_dockerClient.InspectContainer(m_id).data()).release();
    }
    catch (const DockerHTTPException& e)
    {
        THROW_HR_MSG(E_FAIL, "Failed to inspect container: %hs ", e.what());
    }
}

WSLAContainer::WSLAContainer(WSLAContainerImpl* impl, std::function<void(const WSLAContainerImpl*)>&& OnDeleted) :
    COMImplClass<WSLAContainerImpl>(impl), m_onDeleted(std::move(OnDeleted))
{
}

HRESULT WSLAContainer::GetState(WSLA_CONTAINER_STATE* Result)
{
    *Result = WslaContainerStateInvalid;
    return CallImpl(&WSLAContainerImpl::GetState, Result);
}

HRESULT WSLAContainer::GetInitProcess(IWSLAProcess** Process)
{
    *Process = nullptr;
    return CallImpl(&WSLAContainerImpl::GetInitProcess, Process);
}

HRESULT WSLAContainer::Exec(const WSLA_PROCESS_OPTIONS* Options, IWSLAProcess** Process, int* Errno)
{
    *Errno = -1;
    return CallImpl(&WSLAContainerImpl::Exec, Options, Process, Errno);
}

HRESULT WSLAContainer::Stop(int Signal, ULONG TimeoutMs)
{
    return CallImpl(&WSLAContainerImpl::Stop, Signal, TimeoutMs);
}

HRESULT WSLAContainer::Start()
{
    return CallImpl(&WSLAContainerImpl::Start);
}

HRESULT WSLAContainer::Inspect(LPSTR* Output)
{
    *Output = nullptr;

    return CallImpl(&WSLAContainerImpl::Inspect, Output);
}

HRESULT WSLAContainer::Delete()
try
{
    // Special case for Delete(): If deletion is successful, notify the WSLASession that the container has been deleted.
    auto [lock, impl] = LockImpl();

    impl->Delete();
    m_onDeleted(impl);

    return S_OK;
}
CATCH_RETURN();