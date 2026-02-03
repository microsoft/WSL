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

using wsl::windows::common::relay::DockerIORelayHandle;
using wsl::windows::common::relay::HandleWrapper;
using wsl::windows::common::relay::HTTPChunkBasedReadHandle;
using wsl::windows::common::relay::OverlappedIOHandle;
using wsl::windows::common::relay::ReadHandle;
using wsl::windows::common::relay::RelayHandle;
using wsl::windows::service::wsla::VolumeMountInfo;
using wsl::windows::service::wsla::WSLAContainer;
using wsl::windows::service::wsla::WSLAContainerImpl;
using wsl::windows::service::wsla::WSLAVirtualMachine;

using namespace wsl::windows::common::docker_schema;
namespace wsla_schema = wsl::windows::common::wsla_schema;

using DockerInspectContainer = wsl::windows::common::docker_schema::InspectContainer;
using WslaInspectContainer = wsl::windows::common::wsla_schema::InspectContainer;

namespace {

std::vector<std::string> StringArrayToVector(const WSLAStringArray& array)
{
    if (array.Count == 0)
    {
        return {};
    }
    else
    {
        return {&array.Values[0], &array.Values[array.Count]};
    }
}

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

std::string ExtractContainerName(const std::vector<std::string>& names, const std::string& id)
{
    if (names.empty())
    {
        return id;
    }

    // Docker container names have a leading '/', strip it.
    std::string name = names[0];
    if (!name.empty() && name[0] == '/')
    {
        name = name.substr(1);
    }

    return name;
}

} // namespace

static constexpr DWORD stopTimeout = 60000; // 60 seconds

WSLAContainerImpl::WSLAContainerImpl(
    WSLAVirtualMachine* parentVM,
    std::string&& Id,
    std::string&& Name,
    std::string&& Image,
    std::vector<VolumeMountInfo>&& volumes,
    std::vector<PortMapping>&& ports,
    std::function<void(const WSLAContainerImpl*)>&& onDeleted,
    ContainerEventTracker& EventTracker,
    DockerHTTPClient& DockerClient,
    WSLA_CONTAINER_STATE InitialState,
    WSLAProcessFlags InitProcessFlags,
    WSLAContainerFlags ContainerFlags) :
    m_parentVM(parentVM),
    m_name(std::move(Name)),
    m_image(std::move(Image)),
    m_id(std::move(Id)),
    m_mountedVolumes(std::move(volumes)),
    m_mappedPorts(std::move(ports)),
    m_comWrapper(wil::MakeOrThrow<WSLAContainer>(this, std::move(onDeleted))),
    m_dockerClient(DockerClient),
    m_eventTracker(EventTracker),
    m_containerEvents(EventTracker.RegisterContainerStateUpdates(
        m_id, std::bind(&WSLAContainerImpl::OnEvent, this, std::placeholders::_1, std::placeholders::_2))),
    m_state(InitialState),
    m_initProcessFlags(InitProcessFlags),
    m_containerFlags(ContainerFlags)
{
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

    // Stop running containers.
    if (m_state == WslaContainerStateRunning)
    {
        try
        {
            Stop(WSLASignalSIGKILL, stopTimeout);
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

void WSLAContainerImpl::Attach(ULONG* Stdin, ULONG* Stdout, ULONG* Stderr)
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        m_state != WslaContainerStateRunning,
        "Cannot attach to container '%hs', state: %i",
        m_id.c_str(),
        m_state);

    auto ioHandle = m_dockerClient.AttachContainer(m_id);

    // If this is a TTY process, the PTY handle can be returned directly.
    if (WI_IsFlagSet(m_initProcessFlags, WSLAProcessFlagsTty))
    {
        *Stdin = HandleToULong(common::wslutil::DuplicateHandleToCallingProcess(reinterpret_cast<HANDLE>(ioHandle.get())));
        return;
    }

    // Otherwise the stream is multiplexed and needs to be relayed.
    // TODO: Consider skipping stdin if the stdin flag isn't set.
    auto [stdinRead, stdinWrite] = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);
    auto [stdoutRead, stdoutWrite] = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);
    auto [stderrRead, stderrWrite] = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);

    std::vector<std::unique_ptr<OverlappedIOHandle>> handles;

    // This is required for docker to know when stdin is closed.
    auto onInputComplete = [handle = ioHandle.get()]() { LOG_LAST_ERROR_IF(shutdown(handle, SD_SEND) == SOCKET_ERROR); };

    handles.emplace_back(std::make_unique<DockerIORelayHandle>(
        ioHandle.get(), std::move(stdoutWrite), std::move(stderrWrite), DockerIORelayHandle::Format::Raw));

    handles.emplace_back(std::make_unique<RelayHandle<ReadHandle>>(
        HandleWrapper{std::move(stdinRead), std::move(onInputComplete)}, std::move(ioHandle)));

    m_logsRelay.AddHandles(std::move(handles));

    *Stdin = HandleToULong(common::wslutil::DuplicateHandleToCallingProcess(reinterpret_cast<HANDLE>(stdinWrite.get()), GENERIC_WRITE));
    *Stdout = HandleToULong(common::wslutil::DuplicateHandleToCallingProcess(reinterpret_cast<HANDLE>(stdoutRead.get()), GENERIC_READ));
    *Stderr = HandleToULong(common::wslutil::DuplicateHandleToCallingProcess(reinterpret_cast<HANDLE>(stderrRead.get()), GENERIC_READ));
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
    if (WI_IsFlagSet(m_initProcessFlags, WSLAProcessFlagsTty))
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

    try
    {

        m_dockerClient.StartContainer(m_id);
    }
    catch (const DockerHTTPException& e)
    {
        // TODO: wire error back to caller.
        THROW_HR_MSG(E_FAIL, "Failed to start container '%hs': %hs", m_id.c_str(), e.what());
    }

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

void WSLAContainerImpl::Stop(WSLASignal Signal, LONGLONG TimeoutSeconds)
{
    std::lock_guard lock(m_lock);

    if (State() == WslaContainerStateExited)
    {
        return;
    }

    try
    {
        std::optional<WSLASignal> SignalArg;
        if (Signal != WSLASignalNone)
        {
            SignalArg = Signal;
        }

        std::optional<ULONG> TimeoutArg;
        if (TimeoutSeconds >= 0)
        {
            TimeoutArg = static_cast<ULONG>(TimeoutSeconds);
        }

        m_dockerClient.StopContainer(m_id, SignalArg, TimeoutArg);
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
        State() == WslaContainerStateRunning,
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
    THROW_HR_IF_MSG(E_INVALIDARG, Options->CommandLine.Count == 0, "Exec command line cannot be empty");

    std::lock_guard lock{m_lock};

    auto state = State();
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        state != WslaContainerStateRunning,
        "Container %hs is not running. State: %i",
        m_name.c_str(),
        state);

    common::docker_schema::CreateExec request{};
    request.AttachStdout = true;
    request.AttachStderr = true;

    request.Cmd = StringArrayToVector(Options->CommandLine);
    request.Env = StringArrayToVector(Options->Environment);

    if (Options->CurrentDirectory != nullptr)
    {
        request.WorkingDir = Options->CurrentDirectory;
    }

    if (Options->User != nullptr)
    {
        request.User = Options->User;
    }

    if (WI_IsFlagSet(Options->Flags, WSLAProcessFlagsTty))
    {
        request.Tty = true;
    }

    if (WI_IsFlagSet(Options->Flags, WSLAProcessFlagsStdin))
    {
        request.AttachStdin = true;
    }

    try
    {
        auto result = m_dockerClient.CreateExec(m_id, request);

        // N.B. There's no way to delete a created exec instance, it is removed when the container is deleted.

        wil::unique_handle stream{
            (HANDLE)m_dockerClient
                .StartExec(result.Id, common::docker_schema::StartExec{.Tty = request.Tty, .ConsoleSize = request.ConsoleSize})
                .release()};
        std::unique_ptr<WSLAProcessIO> io;
        if (request.Tty)
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

WslaInspectContainer WSLAContainerImpl::BuildInspectContainer(const DockerInspectContainer& dockerInspect)
{
    WslaInspectContainer wslaInspect{};

    wslaInspect.Id = dockerInspect.Id;
    wslaInspect.Name = dockerInspect.Name;

    // Remove leading '/' from Docker container names.
    if (!wslaInspect.Name.empty() && wslaInspect.Name[0] == '/')
    {
        wslaInspect.Name = wslaInspect.Name.substr(1);
    }

    wslaInspect.Created = dockerInspect.Created;
    wslaInspect.Image = dockerInspect.Image;

    // Map container state.
    wslaInspect.State.Status = dockerInspect.State.Status;
    wslaInspect.State.Running = dockerInspect.State.Running;
    wslaInspect.State.ExitCode = dockerInspect.State.ExitCode;
    wslaInspect.State.StartedAt = dockerInspect.State.StartedAt;
    wslaInspect.State.FinishedAt = dockerInspect.State.FinishedAt;

    wslaInspect.HostConfig.NetworkMode = dockerInspect.HostConfig.NetworkMode;

    // Map WSLA port mappings (Windows host ports only). HostIp is not surfaced.
    for (const auto& e : m_mappedPorts)
    {
        auto portKey = std::format("{}/tcp", e.ContainerPort);

        wsla_schema::InspectPortBinding portBinding{};
        portBinding.HostPort = std::to_string(e.HostPort);

        wslaInspect.Ports[portKey].push_back(std::move(portBinding));
    }

    // Map volume mounts using WSLA's host-side data.
    wslaInspect.Mounts.reserve(m_mountedVolumes.size());
    for (const auto& volume : m_mountedVolumes)
    {
        wsla_schema::InspectMount mountInfo{};
        mountInfo.Type = "bind";
        mountInfo.Source = wsl::shared::string::WideToMultiByte(volume.HostPath);
        mountInfo.Destination = volume.ContainerPath;
        mountInfo.ReadWrite = !volume.ReadOnly;
        wslaInspect.Mounts.push_back(std::move(mountInfo));
    }

    return wslaInspect;
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
    common::docker_schema::CreateContainer request;
    request.Image = containerOptions.Image;

    if (WI_IsFlagSet(containerOptions.InitProcessOptions.Flags, WSLAProcessFlagsTty))
    {
        request.Tty = true;
    }

    if (WI_IsFlagSet(containerOptions.InitProcessOptions.Flags, WSLAProcessFlagsStdin))
    {
        request.OpenStdin = true;
        request.StdinOnce = true;
    }

    request.Cmd = StringArrayToVector(containerOptions.InitProcessOptions.CommandLine);
    request.Entrypoint = StringArrayToVector(containerOptions.Entrypoint);
    request.Env = StringArrayToVector(containerOptions.InitProcessOptions.Environment);

    if (containerOptions.StopSignal != WSLASignalNone)
    {
        request.StopSignal = std::to_string(containerOptions.StopSignal);
    }

    if (containerOptions.InitProcessOptions.CurrentDirectory != nullptr)
    {
        request.WorkingDir = containerOptions.InitProcessOptions.CurrentDirectory;
    }

    if (containerOptions.HostName != nullptr)
    {
        request.Hostname = containerOptions.HostName;
    }

    if (containerOptions.DomainName != nullptr)
    {
        request.Domainname = containerOptions.DomainName;
    }

    if (containerOptions.InitProcessOptions.User != nullptr)
    {
        request.User = containerOptions.InitProcessOptions.User;
    }

    request.HostConfig.Init = WI_IsFlagSet(containerOptions.Flags, WSLAContainerFlagsInit);

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
        &parentVM,
        std::move(result.Id),
        std::move(std::string(containerOptions.Name == nullptr ? "" : containerOptions.Name)),
        std::move(std::string(containerOptions.Image)),
        std::move(volumes),
        std::vector<PortMapping>(*mappedPorts),
        std::move(OnDeleted),
        EventTracker,
        DockerClient,
        WslaContainerStateCreated,
        containerOptions.InitProcessOptions.Flags,
        containerOptions.Flags);

    errorCleanup.release();

    return container;
}

std::unique_ptr<WSLAContainerImpl> WSLAContainerImpl::Open(
    const common::docker_schema::ContainerInfo& dockerContainer,
    WSLAVirtualMachine& parentVM,
    std::function<void(const WSLAContainerImpl*)>&& OnDeleted,
    ContainerEventTracker& EventTracker,
    DockerHTTPClient& DockerClient)
{
    // Extract container name from Docker's names list.
    std::string name = ExtractContainerName(dockerContainer.Names, dockerContainer.Id);

    // Convert Docker ports to PortMapping.
    // TODO: Recover host port mapping info from metadata.
    std::vector<PortMapping> ports;
    for (const auto& port : dockerContainer.Ports)
    {
        if (port.PublicPort != 0)
        {
            ports.push_back({port.PublicPort, port.PublicPort, port.PrivatePort, AF_INET, false});
        }
    }

    // Create a WSLAContainerImpl directly without going through Create().
    // TODO: Recover TTY from metadata.
    // TODO: Recover volumes from metadata.
    // TODO: Recover container state from Docker.
    // For now, assume the container has exited since we can't run containers in the new session until
    // we can recover all the necessary state.
    auto container = std::make_unique<WSLAContainerImpl>(
        &parentVM,
        std::string(dockerContainer.Id),
        std::move(name),
        std::string(dockerContainer.Image),
        std::vector<VolumeMountInfo>{},
        std::move(ports),
        std::move(OnDeleted),
        EventTracker,
        DockerClient,
        WslaContainerStateExited,
        WSLAProcessFlagsNone, // TODO
        WSLAContainerFlagsNone);

    WSL_LOG(
        "ContainerOpened",
        TraceLoggingValue(container->m_name.c_str(), "Name"),
        TraceLoggingValue(container->m_id.c_str(), "Id"),
        TraceLoggingValue((int)container->m_state, "State"));

    return container;
}

const std::string& WSLAContainerImpl::ID() const noexcept
{
    return m_id;
}

void WSLAContainerImpl::Inspect(LPSTR* Output)
{
    std::lock_guard lock(m_lock);

    try
    {
        // Get Docker inspect data
        auto dockerJson = m_dockerClient.InspectContainer(m_id);
        auto dockerInspect = wsl::shared::FromJson<DockerInspectContainer>(dockerJson.c_str());

        // Convert to WSLA schema
        auto wslaInspect = BuildInspectContainer(dockerInspect);

        // Serialize WSLA schema to JSON
        std::string wslaJson = wsl::shared::ToJson(wslaInspect);
        *Output = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(wslaJson.c_str()).release();
    }
    catch (const DockerHTTPException& e)
    {
        THROW_HR_MSG(E_FAIL, "Failed to inspect container: %hs ", e.what());
    }
}

void WSLAContainerImpl::Logs(WSLALogsFlags Flags, ULONG* Stdout, ULONG* Stderr, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail)
{
    std::lock_guard lock(m_lock);

    wil::unique_socket socket;
    try
    {
        socket = m_dockerClient.ContainerLogs(m_id, Flags, Since, Until, Tail);
    }
    catch (const DockerHTTPException& e)
    {
        THROW_HR_MSG(E_FAIL, "Failed to get container logs: %hs ", e.what());
    }

    if (WI_IsFlagSet(m_initProcessFlags, WSLAProcessFlagsTty))
    {
        // For tty processes, simply relay the HTTP chunks.
        auto [ttyRead, ttyWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);

        auto handle = std::make_unique<RelayHandle<HTTPChunkBasedReadHandle>>(std::move(socket), std::move(ttyWrite));
        m_logsRelay.AddHandle(std::move(handle));

        *Stdout = HandleToULong(common::wslutil::DuplicateHandleToCallingProcess(ttyRead.get()));
    }
    else
    {
        // For non-tty process, stdout & stderr are multiplexed.
        auto [stdoutRead, stdoutWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);
        auto [stderrRead, stderrWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);

        auto handle = std::make_unique<DockerIORelayHandle>(
            std::move(socket), std::move(stdoutWrite), std::move(stderrWrite), DockerIORelayHandle::Format::HttpChunked);

        m_logsRelay.AddHandle(std::move(handle));

        *Stdout = HandleToULong(common::wslutil::DuplicateHandleToCallingProcess(stdoutRead.get()));
        *Stderr = HandleToULong(common::wslutil::DuplicateHandleToCallingProcess(stderrRead.get()));
    }
}

WSLAContainer::WSLAContainer(WSLAContainerImpl* impl, std::function<void(const WSLAContainerImpl*)>&& OnDeleted) :
    COMImplClass<WSLAContainerImpl>(impl), m_onDeleted(std::move(OnDeleted))
{
}

HRESULT WSLAContainer::Attach(ULONG* Stdin, ULONG* Stdout, ULONG* Stderr)
{
    *Stdin = 0;
    *Stdout = 0;
    *Stderr = 0;

    return CallImpl(&WSLAContainerImpl::Attach, Stdin, Stdout, Stderr);
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

HRESULT WSLAContainer::Stop(_In_ WSLASignal Signal, _In_ LONGLONG TimeoutSeconds)
{
    return CallImpl(&WSLAContainerImpl::Stop, Signal, TimeoutSeconds);
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

HRESULT WSLAContainer::Logs(WSLALogsFlags Flags, ULONG* Stdout, ULONG* Stderr, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail)
{
    RETURN_HR_IF(E_POINTER, Stdout == nullptr || Stderr == nullptr);

    *Stdout = 0;
    *Stderr = 0;

    return CallImpl(&WSLAContainerImpl::Logs, Flags, Stdout, Stderr, Since, Until, Tail);
}

HRESULT WSLAContainer::GetId(WSLAContainerId Id)
try
{
    auto [lock, impl] = LockImpl();
    WI_VERIFY(strcpy_s(Id, std::size<char>(WSLAContainerId{}), impl->ID().c_str()) == 0);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAContainer::GetName(LPSTR* Name)
try
{
    *Name = nullptr;

    auto [lock, impl] = LockImpl();

    *Name = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(impl->Name().c_str()).release();

    return S_OK;
}
CATCH_RETURN();