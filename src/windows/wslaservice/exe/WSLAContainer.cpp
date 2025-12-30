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

using wsl::windows::service::wsla::VolumeMountInfo;
using wsl::windows::service::wsla::WSLAContainer;
using wsl::windows::service::wsla::WSLAContainerImpl;
using wsl::windows::service::wsla::WSLAVirtualMachine;

// Constants for required default arguments for "nerdctl create..."
static std::vector<std::string> defaultNerdctlCreateArgs{//"--pull=never", // TODO: Uncomment once PullImage() is implemented.
                                                         "--ulimit",
                                                         "nofile=65536:65536"};

static std::vector<std::string> defaultNerdctlEnv{"PATH=/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/sbin"};

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
                LOG_IF_FAILED_MSG(
                    vm.MapPort(e.Family, e.HostPort, e.VmPort, true),
                    "Failed to unmap port (family=%i, guestPort=%u, hostPort=%u)",
                    e.Family,
                    e.VmPort,
                    e.HostPort);
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
        THROW_IF_FAILED(vm.MapPort(e.Family, e.HostPort, e.VmPort, false));
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
    m_name(Options.Name),
    m_image(Options.Image),
    m_id(std::move(Id)),
    m_mountedVolumes(std::move(volumes)),
    m_mappedPorts(std::move(ports)),
    m_comWrapper(wil::MakeOrThrow<WSLAContainer>(this, std::move(onDeleted))),
    m_dockerClient(DockerClient),
    m_containerEvents(EventTracker.RegisterContainerStateUpdates(m_id, std::bind(&WSLAContainerImpl::OnEvent, this, std::placeholders::_1)))
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

        LOG_IF_FAILED_MSG(
            m_parentVM->MapPort(e.Family, e.HostPort, e.VmPort, true),
            "Failed to delete port mapping (family=%i, guestPort=%u, hostPort=%u)",
            e.Family,
            e.VmPort,
            e.HostPort);

        allocatedGuestPorts.insert(e.VmPort);
    }

    m_parentVM->ReleasePorts(allocatedGuestPorts);
}

const std::string& WSLAContainerImpl::Image() const noexcept
{
    return m_image;
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
    m_initProcess = wil::MakeOrThrow<WSLAContainerProcess>(
        m_id, wil::unique_handle{(HANDLE)m_dockerClient.AttachContainer(m_id).release()}, m_tty, m_dockerClient);

    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this]() mutable { m_initProcess.Reset(); });

    m_dockerClient.StartContainer(m_id);

    m_state = WslaContainerStateRunning;
    cleanup.release();
}

void WSLAContainerImpl::OnEvent(ContainerEvent event)
{
    if (event == ContainerEvent::Stop)
    {
        std::lock_guard<std::recursive_mutex> lock(m_lock);
        m_state = WslaContainerStateExited;

        // TODO: propagate exit code.
        m_initProcess->OnExited(0);
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

    // If the container is running, refresh the init process state before returning.
    if (m_state == WslaContainerStateRunning && m_initProcess->State().first != WSLAProcessStateRunning)
    {
        m_state = WslaContainerStateExited;
        // m_containerProcess.reset();
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

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        State() != WslaContainerStateRunning,
        "Container %hs is not running. State: %i",
        m_name.c_str(),
        m_state);

    auto [hasStdin, hasTty] = ParseFdStatus(*Options);

    std::vector<std::string> args{nerdctlPath, "exec"};

    if (hasStdin)
    {
        args.push_back("-i");
    }

    if (hasTty)
    {
        args.push_back("-t");
    }

    AddEnvironmentVariables(args, *Options);

    args.emplace_back(m_id);

    for (ULONG i = 0; i < Options->CommandLineCount; i++)
    {
        args.emplace_back(Options->CommandLine[i]);
    }

    ServiceProcessLauncher launcher(nerdctlPath, args, defaultNerdctlEnv, common::ProcessFlags::None);
    for (auto i = 0; i < Options->FdsCount; i++)
    {
        launcher.AddFd(Options->Fds[i]);
    }

    std::optional<ServiceRunningProcess> process;
    HRESULT result = E_FAIL;
    std::tie(result, *Errno, process) = launcher.LaunchNoThrow(*m_parentVM);
    THROW_IF_FAILED(result);

    THROW_IF_FAILED(process->Get().QueryInterface(__uuidof(IWSLAProcess), (void**)Process));
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

    docker_schema::CreateContainer request;
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
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            containerOptions.InitProcessOptions.Environment[i][0] == '-',
            "Invalid environment string at index: %i: %hs",
            i,
            containerOptions.InitProcessOptions.Environment[i]);

        request.Env.push_back(containerOptions.InitProcessOptions.Environment[i]);
    }

    // Mount volumes.
    auto volumes = MountVolumes(containerOptions, parentVM);

    for (const auto& e : volumes)
    {
        request.HostConfig.Mounts.emplace_back(
            docker_schema::Mount{.Source = e.ParentVMPath, .Target = e.ContainerPath, .Type = "bind", .ReadOnly = e.ReadOnly});
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
        auto& portEntry = request.HostConfig.PortBindings[std::format("{}/tcp", e.ContainerPort)];
        portEntry.emplace_back(docker_schema::PortMapping{.HostIp = "127.0.0.1", .HostPort = std::to_string(e.VmPort)});
    }

    // Send the request to docker.
    try
    {

        auto result = DockerClient.CreateContainer(request);

        // TODO: Rethink command line generation logic.

        // N.B. mappedPorts is explicitly copied because it's referenced in errorCleanup, so it can't be moved.
        auto container = std::make_unique<WSLAContainerImpl>(
            &parentVM, containerOptions, std::move(result.Id), std::move(volumes), std::vector<PortMapping>(*mappedPorts), std::move(OnDeleted), EventTracker, DockerClient);

        errorCleanup.release();

        return container;
    }
    catch (const DockerHTTPException& e)
    {
        // TODO: propagate error message to caller.
        THROW_HR_MSG(E_FAIL, "Failed to create container: %hs ", e.what());
    }
}

std::vector<std::string> WSLAContainerImpl::PrepareNerdctlCreateCommand(
    const WSLA_CONTAINER_OPTIONS& options, std::vector<std::string>&& inputOptions, std::vector<VolumeMountInfo>& volumes)
{
    std::vector<std::string> args{nerdctlPath};
    args.push_back("create");
    args.push_back("--name");
    args.push_back(options.Name);

    switch (options.ContainerNetwork.ContainerNetworkType)
    {
    case WSLA_CONTAINER_NETWORK_HOST:
        args.push_back("--net=host");
        break;
    case WSLA_CONTAINER_NETWORK_NONE:
        args.push_back("--net=none");
        break;
    case WSLA_CONTAINER_NETWORK_BRIDGE:
        args.push_back("--net=bridge");
        break;
    // TODO: uncomment and implement when we have custom networks
    // case WSLA_CONTAINER_NETWORK_CUSTOM:
    //     args.push_back(std::format("--net={}", options.ContainerNetwork.ContainerNetworkName));
    //     break;
    default:
        THROW_HR_MSG(
            E_INVALIDARG,
            "No such network: type: %i, name: %hs",
            options.ContainerNetwork.ContainerNetworkType,
            options.ContainerNetwork.ContainerNetworkName);
        break;
    }

    if (options.ShmSize > 0)
    {
        args.push_back(std::format("--shm-size={}m", options.ShmSize));
    }
    if (options.Flags & WSLA_CONTAINER_FLAG_ENABLE_GPU)
    {
        args.push_back("--gpus");
        // TODO: Parse GPU device list from WSLA_CONTAINER_OPTIONS. For now, just enable all GPUs.
        args.push_back("all");
    }

    args.insert(args.end(), defaultNerdctlCreateArgs.begin(), defaultNerdctlCreateArgs.end());
    args.insert(args.end(), inputOptions.begin(), inputOptions.end());

    if (options.InitProcessOptions.Executable != nullptr)
    {
        args.push_back("--entrypoint");
        args.push_back(options.InitProcessOptions.Executable);
    }

    for (const auto& volume : volumes)
    {
        args.emplace_back(std::format("-v{}:{}{}", volume.ParentVMPath, volume.ContainerPath, volume.ReadOnly ? ":ro" : ""));
    }

    // TODO:
    // - Implement port mapping

    args.push_back(options.Image);

    if (options.InitProcessOptions.CommandLineCount > 0)
    {
        args.push_back("--");

        for (ULONG i = 0; i < options.InitProcessOptions.CommandLineCount; i++)
        {
            args.push_back(options.InitProcessOptions.CommandLine[i]);
        }
    }

    return args;
}

WSLAContainer::WSLAContainer(WSLAContainerImpl* impl, std::function<void(const WSLAContainerImpl*)>&& OnDeleted) :
    m_impl(impl), m_onDeleted(std::move(OnDeleted))
{
}

void WSLAContainer::Disconnect() noexcept
{
    std::lock_guard lock(m_lock);

    WI_ASSERT(m_impl != nullptr);
    m_impl = nullptr;
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

HRESULT WSLAContainer::Delete()
try
{
    // Special case for Delete(): If deletion is successful, notify the WSLASession that the container has been deleted.
    std::lock_guard lock{m_lock};
    RETURN_HR_IF(RPC_E_DISCONNECTED, m_impl == nullptr);

    m_impl->Delete();
    m_onDeleted(m_impl);

    return S_OK;
}
CATCH_RETURN();