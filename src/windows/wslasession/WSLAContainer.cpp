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

using wsl::windows::common::COMServiceExecutionContext;
using wsl::windows::common::docker_schema::ErrorResponse;
using wsl::windows::common::relay::DockerIORelayHandle;
using wsl::windows::common::relay::HandleWrapper;
using wsl::windows::common::relay::HTTPChunkBasedReadHandle;
using wsl::windows::common::relay::OverlappedIOHandle;
using wsl::windows::common::relay::ReadHandle;
using wsl::windows::common::relay::RelayHandle;
using wsl::windows::service::wsla::RelayedProcessIO;
using wsl::windows::service::wsla::WSLAContainer;
using wsl::windows::service::wsla::WSLAContainerImpl;
using wsl::windows::service::wsla::WSLAContainerMetadata;
using wsl::windows::service::wsla::WSLAContainerMetadataV1;
using wsl::windows::service::wsla::WSLAPortMapping;
using wsl::windows::service::wsla::WSLAVirtualMachine;
using wsl::windows::service::wsla::WSLAVolumeMount;

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

auto ValidatePortMappings(const WSLA_CONTAINER_OPTIONS& options)
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
}

auto MapPorts(std::vector<WSLAPortMapping>& ports, WSLAVirtualMachine& vm)
{
    // N.B. pointers are used so the vectors are still available if the errorCleanup is executed.
    auto vmPorts = std::make_shared<std::set<uint16_t>>();

    auto errorCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&ports, vmPorts = vmPorts, &vm]() {
        if (!vmPorts->empty())
        {
            LOG_IF_FAILED(wil::ResultFromException([&]() { vm.ReleasePorts(*vmPorts); }));
        }

        for (const auto& e : ports)
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

    // Check if we need to allocate VM ports for bridge mode (VmPort == 0).
    size_t portsToAllocate = std::count_if(ports.begin(), ports.end(), [](const auto& p) { return p.VmPort == 0; });

    if (portsToAllocate > 0)
    {
        auto allocatedPorts = vm.AllocatePorts(static_cast<uint16_t>(portsToAllocate));
        auto allocatedIt = allocatedPorts.begin();

        for (auto& port : ports)
        {
            if (port.VmPort == 0)
            {
                WI_ASSERT(allocatedIt != allocatedPorts.end());
                port.VmPort = *allocatedIt;
                vmPorts->insert(*allocatedIt);
                ++allocatedIt;
            }
        }
    }

    // In host mode, the VM ports are the same as the container ports. Ensure they are allocated.
    for (const auto& port : ports)
    {
        // Only allocate a VM port if it hasn't already been allocated to that container.
        // A user can allocate two different host ports to the same container port.
        if (vmPorts->find(port.VmPort) == vmPorts->end())
        {
            THROW_WIN32_IF_MSG(ERROR_ALREADY_EXISTS, !vm.TryAllocatePort(port.VmPort), "Failed to allocate port: %u", port.VmPort);
            vmPorts->insert(port.VmPort);
        }
    }

    // Map Windows <-> VM ports.
    for (auto& e : ports)
    {
        vm.MapPort(e.Family, e.HostPort, e.VmPort);
        e.MappedToHost = true;
    }

    return std::make_pair(std::move(vmPorts), std::move(errorCleanup));
}

// Builds port mapping list from container options and returns the network mode string.
// Note: For bridge mode, VM ports are set to 0 and will be allocated later by MapPorts().
std::pair<std::vector<WSLAPortMapping>, std::string> ProcessPortMappings(const WSLA_CONTAINER_OPTIONS& options)
{
    WSLA_CONTAINER_NETWORK_TYPE networkType = options.ContainerNetwork.ContainerNetworkType;

    // Determine network mode string.
    std::string networkMode;
    if (networkType == WSLA_CONTAINER_NETWORK_BRIDGE)
    {
        networkMode = "bridge";
    }
    else if (networkType == WSLA_CONTAINER_NETWORK_HOST)
    {
        networkMode = "host";
    }
    else if (networkType == WSLA_CONTAINER_NETWORK_NONE)
    {
        networkMode = "none";
    }
    else
    {
        THROW_HR_MSG(E_INVALIDARG, "Invalid networking mode: %i", networkType);
    }

    // Validate port mappings.
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        options.PortsCount > 0 && networkType == WSLA_CONTAINER_NETWORK_NONE,
        "Port mappings are not supported without networking");

    std::vector<WSLAPortMapping> ports;
    ports.reserve(options.PortsCount);

    for (ULONG i = 0; i < options.PortsCount; i++)
    {
        const auto& port = options.Ports[i];
        THROW_HR_IF_MSG(E_INVALIDARG, port.Family != AF_INET && port.Family != AF_INET6, "Invalid family for port mapping %i: %i", i, port.Family);

        if (networkType == WSLA_CONTAINER_NETWORK_BRIDGE)
        {
            // In bridged mode, VM port will be allocated by MapPorts() - set to 0 as placeholder.
            ports.push_back({port.HostPort, 0, port.ContainerPort, port.Family});
        }
        else if (networkType == WSLA_CONTAINER_NETWORK_HOST)
        {
            // In host mode, the container port is the same as the VM port.
            ports.push_back({port.HostPort, port.ContainerPort, port.ContainerPort, port.Family});
        }
    }

    return {std::move(ports), std::move(networkMode)};
}

void UnmountVolumes(const std::vector<WSLAVolumeMount>& volumes, WSLAVirtualMachine& parentVM)
{
    for (const auto& volume : volumes)
    {
        LOG_IF_FAILED(parentVM.UnmountWindowsFolder(volume.ParentVMPath.c_str()));
    }
}

auto MountVolumes(std::vector<WSLAVolumeMount>& volumes, WSLAVirtualMachine& parentVM)
{
    auto errorCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&volumes, &parentVM]() { UnmountVolumes(volumes, parentVM); });

    for (auto& volume : volumes)
    {
        auto result = parentVM.MountWindowsFolder(volume.HostPath.c_str(), volume.ParentVMPath.c_str(), volume.ReadOnly);
        THROW_IF_FAILED_MSG(result, "Failed to mount %ls -> %hs", volume.HostPath.c_str(), volume.ParentVMPath.c_str());
    }

    return std::move(errorCleanup);
}

WSLA_CONTAINER_STATE DockerStateToWSLAState(ContainerState state)
{
    // TODO: Handle other states like Paused, Restarting, etc.
    switch (state)
    {
    case ContainerState::Created:
        return WSLA_CONTAINER_STATE::WslaContainerStateCreated;
    case ContainerState::Running:
        return WSLA_CONTAINER_STATE::WslaContainerStateRunning;
    case ContainerState::Exited:
    case ContainerState::Dead:
        return WSLA_CONTAINER_STATE::WslaContainerStateExited;
    case ContainerState::Removing:
        return WSLA_CONTAINER_STATE::WslaContainerStateDeleted;
    default:
        return WSLA_CONTAINER_STATE::WslaContainerStateInvalid;
    }
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

WSLAContainerMetadataV1 ParseContainerMetadata(const std::string& json)
{
    auto wrapper = wsl::shared::FromJson<WSLAContainerMetadata>(json.c_str());
    THROW_HR_IF(E_UNEXPECTED, !wrapper.V1.has_value());

    return wrapper.V1.value();
}

std::string SerializeContainerMetadata(const WSLAContainerMetadataV1& metadata)
{
    WSLAContainerMetadata wrapper;
    wrapper.V1 = metadata;

    return wsl::shared::ToJson(wrapper);
}

} // namespace

WSLAContainerImpl::WSLAContainerImpl(
    WSLAVirtualMachine* parentVM,
    std::string&& Id,
    std::string&& Name,
    std::string&& Image,
    std::vector<WSLAVolumeMount>&& volumes,
    std::vector<WSLAPortMapping>&& ports,
    std::map<std::string, std::string>&& labels,
    std::function<void(const WSLAContainerImpl*)>&& onDeleted,
    ContainerEventTracker& EventTracker,
    DockerHTTPClient& DockerClient,
    IORelay& Relay,
    WSLA_CONTAINER_STATE InitialState,
    WSLAProcessFlags InitProcessFlags,
    WSLAContainerFlags ContainerFlags) :
    m_parentVM(parentVM),
    m_name(std::move(Name)),
    m_image(std::move(Image)),
    m_id(std::move(Id)),
    m_mountedVolumes(std::move(volumes)),
    m_mappedPorts(std::move(ports)),
    m_labels(std::move(labels)),
    m_comWrapper(wil::MakeOrThrow<WSLAContainer>(this, std::move(onDeleted))),
    m_dockerClient(DockerClient),
    m_eventTracker(EventTracker),
    m_ioRelay(Relay),
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
    ReleaseResources();
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

    wil::unique_socket ioHandle;

    try
    {
        ioHandle = m_dockerClient.AttachContainer(m_id);
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to attach to container '%hs'", m_id.c_str());

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

    // N.B. Ownership of the io handle is given to the DockerIORelayHandle relay, so it can be closed when docker closes the connection.
    handles.emplace_back(
        std::make_unique<RelayHandle<ReadHandle>>(HandleWrapper{std::move(stdinRead), std::move(onInputComplete)}, ioHandle.get()));

    handles.emplace_back(std::make_unique<DockerIORelayHandle>(
        std::move(ioHandle), std::move(stdoutWrite), std::move(stderrWrite), DockerIORelayHandle::Format::Raw));

    m_ioRelay.AddHandles(std::move(handles));

    *Stdin = HandleToULong(common::wslutil::DuplicateHandleToCallingProcess(reinterpret_cast<HANDLE>(stdinWrite.get())));
    *Stdout = HandleToULong(common::wslutil::DuplicateHandleToCallingProcess(reinterpret_cast<HANDLE>(stdoutRead.get())));
    *Stderr = HandleToULong(common::wslutil::DuplicateHandleToCallingProcess(reinterpret_cast<HANDLE>(stderrRead.get())));
}

void WSLAContainerImpl::Start(WSLAContainerStartFlags Flags)
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

    if (WI_IsFlagSet(Flags, WSLAContainerStartFlagsAttach))
    {
        if (WI_IsFlagSet(m_initProcessFlags, WSLAProcessFlagsTty))
        {
            io = std::make_unique<TTYProcessIO>(wil::unique_handle{(HANDLE)m_dockerClient.AttachContainer(m_id).release()});
        }
        else
        {
            wil::unique_handle stream{reinterpret_cast<HANDLE>(m_dockerClient.AttachContainer(m_id).release())};
            io = CreateRelayedProcessIO(std::move(stream), m_initProcessFlags);
        }
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
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to start container '%hs'", m_id.c_str());

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

        if (WI_IsFlagSet(m_containerFlags, WSLAContainerFlagsRm))
        {
            Delete();
        }
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
        if (e.StatusCode() != 304)
        {
            THROW_DOCKER_USER_ERROR_MSG(e, "Failed to stop container '%hs'", m_id.c_str());
        }
    }

    m_state = WslaContainerStateExited;

    if (WI_IsFlagSet(m_containerFlags, WSLAContainerFlagsRm))
    {
        Delete();
    }
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

    try
    {
        m_dockerClient.DeleteContainer(m_id);
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to delete container '%hs'", m_id.c_str());

    ReleaseResources();

    m_state = WslaContainerStateDeleted;
}

WSLA_CONTAINER_STATE WSLAContainerImpl::State() noexcept
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    if (m_state == WslaContainerStateRunning && m_initProcessControl && m_initProcessControl->GetState().first != WslaProcessStateRunning)
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

void WSLAContainerImpl::Exec(const WSLA_PROCESS_OPTIONS* Options, IWSLAProcess** Process)
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
            io = CreateRelayedProcessIO(std::move(stream), Options->Flags);
        }

        auto control = std::make_unique<DockerExecProcessControl>(*this, result.Id, m_dockerClient, m_eventTracker);

        // Store a non owning reference to the process.
        m_processes.push_back(control.get());

        auto process = wil::MakeOrThrow<WSLAProcess>(std::move(control), std::move(io));

        THROW_IF_FAILED(process.CopyTo(__uuidof(IWSLAProcess), (void**)Process));
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to exec process in container %hs", m_id.c_str());
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
    wslaInspect.Image = m_image;

    // Map container state.
    wslaInspect.State.Status = dockerInspect.State.Status;
    wslaInspect.State.Running = dockerInspect.State.Running;
    wslaInspect.State.ExitCode = dockerInspect.State.ExitCode;
    wslaInspect.State.StartedAt = dockerInspect.State.StartedAt;
    wslaInspect.State.FinishedAt = dockerInspect.State.FinishedAt;

    wslaInspect.HostConfig.NetworkMode = dockerInspect.HostConfig.NetworkMode;

    // Map WSLA port mappings (Windows host ports only). HostIp is not set here and will use
    // the default value ("127.0.0.1") defined in the InspectPortBinding schema.
    for (const auto& e : m_mappedPorts)
    {
        // TODO: UDP support
        // TODO: ipv6 support.
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
        // TODO: Support different mount types (plan9/VHD) when VHD volumes are implemented.
        mountInfo.Type = "bind";
        mountInfo.Source = wsl::shared::string::WideToMultiByte(volume.HostPath);
        mountInfo.Destination = volume.ContainerPath;
        mountInfo.ReadWrite = !volume.ReadOnly;
        wslaInspect.Mounts.push_back(std::move(mountInfo));
    }

    return wslaInspect;
}

std::unique_ptr<WSLAContainerImpl> WSLAContainerImpl::Create(
    const WSLA_CONTAINER_OPTIONS& containerOptions,
    WSLAVirtualMachine& parentVM,
    std::function<void(const WSLAContainerImpl*)>&& OnDeleted,
    ContainerEventTracker& EventTracker,
    DockerHTTPClient& DockerClient,
    IORelay& IoRelay)
{
    common::docker_schema::CreateContainer request;
    request.Image = containerOptions.Image;

    // TODO: Think about when 'StdinOnce' should be set.
    request.StdinOnce = true;

    if (WI_IsFlagSet(containerOptions.InitProcessOptions.Flags, WSLAProcessFlagsTty))
    {
        request.Tty = true;
    }

    if (WI_IsFlagSet(containerOptions.InitProcessOptions.Flags, WSLAProcessFlagsStdin))
    {
        request.OpenStdin = true;
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

    // Build volume list from container options.
    std::vector<WSLAVolumeMount> volumes;
    volumes.reserve(containerOptions.VolumesCount);

    for (ULONG i = 0; i < containerOptions.VolumesCount; i++)
    {
        GUID volumeId;
        THROW_IF_FAILED(CoCreateGuid(&volumeId));

        auto parentVMPath = std::format("/mnt/{}", wsl::shared::string::GuidToString<char>(volumeId));
        auto volume = containerOptions.Volumes[i];

        volumes.push_back(WSLAVolumeMount{volume.HostPath, parentVMPath, volume.ContainerPath, static_cast<bool>(volume.ReadOnly)});

        request.HostConfig.Mounts.emplace_back(common::docker_schema::Mount{
            .Source = parentVMPath, .Target = volume.ContainerPath, .Type = "bind", .ReadOnly = static_cast<bool>(volume.ReadOnly)});
    }

    // Mount volumes.
    auto volumeErrorCleanup = MountVolumes(volumes, parentVM);

    // Process port mappings from container options.
    auto [ports, networkMode] = ProcessPortMappings(containerOptions);
    request.HostConfig.NetworkMode = networkMode;

    auto [vmPorts, errorCleanup] = MapPorts(ports, parentVM);

    for (const auto& e : ports)
    {
        // TODO: UDP support
        // TODO: ipv6 support.
        auto portKey = std::format("{}/tcp", e.ContainerPort);
        request.ExposedPorts[portKey] = {};
        auto& portEntry = request.HostConfig.PortBindings[portKey];
        portEntry.emplace_back(common::docker_schema::PortMapping{.HostIp = "127.0.0.1", .HostPort = std::to_string(e.VmPort)});
    }

    std::map<std::string, std::string> labels;
    for (ULONG i = 0; i < containerOptions.LabelsCount; i++)
    {
        const auto& label = containerOptions.Labels[i];

        THROW_HR_IF_NULL_MSG(E_INVALIDARG, label.Key, "Label at index %lu has null key", i);
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, label.Value, "Label at index %lu has null value", i);

        THROW_HR_IF_MSG(E_INVALIDARG, strcmp(label.Key, WSLAContainerMetadataLabel) == 0, "Label key '%hs' is reserved", WSLAContainerMetadataLabel);
        THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), labels.contains(label.Key), "Duplicate label key: '%hs'", label.Key);

        labels[label.Key] = label.Value;
    }

    // Build WSLA metadata to store in a label for recovery on Open().
    WSLAContainerMetadataV1 metadata;
    metadata.Flags = containerOptions.Flags;
    metadata.InitProcessFlags = containerOptions.InitProcessOptions.Flags;
    metadata.Volumes = volumes;
    metadata.Ports = ports;

    request.Labels[WSLAContainerMetadataLabel] = SerializeContainerMetadata(metadata);
    request.Labels.insert(labels.begin(), labels.end());

    // Send the request to docker.
    auto result =
        DockerClient.CreateContainer(request, containerOptions.Name != nullptr ? containerOptions.Name : std::optional<std::string>{});

    auto container = std::make_unique<WSLAContainerImpl>(
        &parentVM,
        std::move(result.Id),
        std::move(std::string(containerOptions.Name == nullptr ? "" : containerOptions.Name)),
        std::move(std::string(containerOptions.Image)),
        std::move(volumes),
        std::move(ports),
        std::move(labels),
        std::move(OnDeleted),
        EventTracker,
        DockerClient,
        IoRelay,
        WslaContainerStateCreated,
        containerOptions.InitProcessOptions.Flags,
        containerOptions.Flags);

    errorCleanup.release();
    volumeErrorCleanup.release();

    return container;
}

std::unique_ptr<WSLAContainerImpl> WSLAContainerImpl::Open(
    const common::docker_schema::ContainerInfo& dockerContainer,
    WSLAVirtualMachine& parentVM,
    std::function<void(const WSLAContainerImpl*)>&& OnDeleted,
    ContainerEventTracker& EventTracker,
    DockerHTTPClient& DockerClient,
    IORelay& ioRelay)
{
    // Extract container name from Docker's names list.
    std::string name = ExtractContainerName(dockerContainer.Names, dockerContainer.Id);

    auto labels(dockerContainer.Labels);
    auto metadataIt = labels.find(WSLAContainerMetadataLabel);

    THROW_HR_IF_MSG(
        E_INVALIDARG,
        metadataIt == labels.end(),
        "Cannot open WSLA container %hs: missing WSLA metadata label",
        dockerContainer.Id.c_str());

    auto metadata = ParseContainerMetadata(metadataIt->second.c_str());
    labels.erase(metadataIt);

    // TODO: Offload volume mounting and port mapping to the Start() method so that its still possible
    // to open containers that are not running.
    auto volumeErrorCleanup = MountVolumes(metadata.Volumes, parentVM);
    auto [vmPorts, errorCleanup] = MapPorts(metadata.Ports, parentVM);

    auto container = std::make_unique<WSLAContainerImpl>(
        &parentVM,
        std::string(dockerContainer.Id),
        std::move(name),
        std::string(dockerContainer.Image),
        std::move(metadata.Volumes),
        std::move(metadata.Ports),
        std::move(labels),
        std::move(OnDeleted),
        EventTracker,
        DockerClient,
        ioRelay,
        DockerStateToWSLAState(dockerContainer.State),
        metadata.InitProcessFlags,
        metadata.Flags);

    errorCleanup.release();
    volumeErrorCleanup.release();

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
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to inspect container '%hs'", m_id.c_str());
}

void WSLAContainerImpl::Logs(WSLALogsFlags Flags, ULONG* Stdout, ULONG* Stderr, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail)
{
    std::lock_guard lock(m_lock);

    wil::unique_socket socket;
    try
    {
        socket = m_dockerClient.ContainerLogs(m_id, Flags, Since, Until, Tail);
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to get logs from '%hs'", m_id.c_str());

    if (WI_IsFlagSet(m_initProcessFlags, WSLAProcessFlagsTty))
    {
        // For tty processes, simply relay the HTTP chunks.
        auto [ttyRead, ttyWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);

        auto handle = std::make_unique<RelayHandle<HTTPChunkBasedReadHandle>>(std::move(socket), std::move(ttyWrite));
        m_ioRelay.AddHandle(std::move(handle));

        *Stdout = HandleToULong(common::wslutil::DuplicateHandleToCallingProcess(ttyRead.get()));
    }
    else
    {
        // For non-tty process, stdout & stderr are multiplexed.
        auto [stdoutRead, stdoutWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);
        auto [stderrRead, stderrWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);

        auto handle = std::make_unique<DockerIORelayHandle>(
            std::move(socket), std::move(stdoutWrite), std::move(stderrWrite), DockerIORelayHandle::Format::HttpChunked);

        m_ioRelay.AddHandle(std::move(handle));

        *Stdout = HandleToULong(common::wslutil::DuplicateHandleToCallingProcess(stdoutRead.get()));
        *Stderr = HandleToULong(common::wslutil::DuplicateHandleToCallingProcess(stderrRead.get()));
    }
}

std::unique_ptr<RelayedProcessIO> WSLAContainerImpl::CreateRelayedProcessIO(wil::unique_handle&& stream, WSLAProcessFlags flags)
{
    // Create one pipe for each STD handle.
    std::vector<std::unique_ptr<OverlappedIOHandle>> ioHandles;
    std::map<ULONG, wil::unique_handle> fds;

    // This is required for docker to know when stdin is closed.
    auto closeStdin = [socket = stream.get(), this]() {
        LOG_LAST_ERROR_IF(shutdown(reinterpret_cast<SOCKET>(socket), SD_SEND) == SOCKET_ERROR);
    };

    if (WI_IsFlagSet(flags, WSLAProcessFlagsStdin))
    {
        auto [stdinRead, stdinWrite] = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);
        ioHandles.emplace_back(
            std::make_unique<RelayHandle<ReadHandle>>(HandleWrapper{std::move(stdinRead), std::move(closeStdin)}, stream.get()));

        fds.emplace(WSLAFDStdin, stdinWrite.release());
    }
    else
    {
        // If stdin is not attached, close it now to make sure no one tries to write to it.
        closeStdin();
    }

    auto [stdoutRead, stdoutWrite] = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);
    auto [stderrRead, stderrWrite] = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);

    fds.emplace(WSLAFDStdout, stdoutRead.release());
    fds.emplace(WSLAFDStderr, stderrRead.release());

    ioHandles.emplace_back(std::make_unique<DockerIORelayHandle>(
        std::move(stream), std::move(stdoutWrite), std::move(stderrWrite), common::relay::DockerIORelayHandle::Format::Raw));

    m_ioRelay.AddHandles(std::move(ioHandles));

    return std::make_unique<RelayedProcessIO>(std::move(fds));
}

void WSLAContainerImpl::ReleaseResources()
{
    std::lock_guard<std::recursive_mutex> lock(m_lock);

    // Disconnect the COM wrapper so no new RPC calls can reach this container.
    if (m_comWrapper)
    {
        m_comWrapper->Disconnect();
        m_comWrapper.Reset();
    }

    // Unmount volumes.
    UnmountVolumes(m_mountedVolumes, *m_parentVM);
    m_mountedVolumes.clear();

    // Unmap and release ports.
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

    if (!allocatedGuestPorts.empty())
    {
        m_parentVM->ReleasePorts(allocatedGuestPorts);
    }

    m_mappedPorts.clear();
}

WSLAContainer::WSLAContainer(WSLAContainerImpl* impl, std::function<void(const WSLAContainerImpl*)>&& OnDeleted) :
    COMImplClass<WSLAContainerImpl>(impl), m_onDeleted(std::move(OnDeleted))
{
}

HRESULT WSLAContainer::Attach(ULONG* Stdin, ULONG* Stdout, ULONG* Stderr)
{
    COMServiceExecutionContext context;

    *Stdin = 0;
    *Stdout = 0;
    *Stderr = 0;

    return CallImpl(&WSLAContainerImpl::Attach, Stdin, Stdout, Stderr);
}

HRESULT WSLAContainer::GetState(WSLA_CONTAINER_STATE* Result)
{
    COMServiceExecutionContext context;

    *Result = WslaContainerStateInvalid;
    return CallImpl(&WSLAContainerImpl::GetState, Result);
}

HRESULT WSLAContainer::GetInitProcess(IWSLAProcess** Process)
{
    COMServiceExecutionContext context;

    *Process = nullptr;
    return CallImpl(&WSLAContainerImpl::GetInitProcess, Process);
}

HRESULT WSLAContainer::Exec(const WSLA_PROCESS_OPTIONS* Options, IWSLAProcess** Process)
{
    COMServiceExecutionContext context;

    *Process = nullptr;
    return CallImpl(&WSLAContainerImpl::Exec, Options, Process);
}

HRESULT WSLAContainer::Stop(_In_ WSLASignal Signal, _In_ LONGLONG TimeoutSeconds)
{
    COMServiceExecutionContext context;

    return CallImpl(&WSLAContainerImpl::Stop, Signal, TimeoutSeconds);
}

HRESULT WSLAContainer::Start(WSLAContainerStartFlags Flags)
{
    COMServiceExecutionContext context;

    return CallImpl(&WSLAContainerImpl::Start, Flags);
}

HRESULT WSLAContainer::Inspect(LPSTR* Output)
{
    COMServiceExecutionContext context;

    *Output = nullptr;

    return CallImpl(&WSLAContainerImpl::Inspect, Output);
}

HRESULT WSLAContainer::Delete()
try
{
    COMServiceExecutionContext context;

    // Special case for Delete(): If deletion is successful, notify the WSLASession that the container has been deleted.
    auto [lock, impl] = LockImpl();

    impl->Delete();
    m_onDeleted(impl);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAContainer::Logs(WSLALogsFlags Flags, ULONG* Stdout, ULONG* Stderr, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail)
{
    COMServiceExecutionContext context;

    RETURN_HR_IF(E_POINTER, Stdout == nullptr || Stderr == nullptr);

    *Stdout = 0;
    *Stderr = 0;

    return CallImpl(&WSLAContainerImpl::Logs, Flags, Stdout, Stderr, Since, Until, Tail);
}

HRESULT WSLAContainer::GetId(WSLAContainerId Id)
try
{
    COMServiceExecutionContext context;

    auto [lock, impl] = LockImpl();
    WI_VERIFY(strcpy_s(Id, std::size<char>(WSLAContainerId{}), impl->ID().c_str()) == 0);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAContainer::GetName(LPSTR* Name)
try
{
    COMServiceExecutionContext context;

    *Name = nullptr;

    auto [lock, impl] = LockImpl();

    *Name = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(impl->Name().c_str()).release();

    return S_OK;
}
CATCH_RETURN();

void WSLAContainerImpl::GetLabels(WSLA_LABEL_INFORMATION** Labels, ULONG* Count)
{
    std::lock_guard lock(m_lock);

    if (m_labels.empty())
    {
        *Labels = nullptr;
        *Count = 0;
        return;
    }

    auto count = m_labels.size();
    auto labelsArray = wil::make_unique_cotaskmem<WSLA_LABEL_INFORMATION[]>(count);

    auto cleanup = wil::scope_exit([&]() {
        for (size_t j = 0; j < count; ++j)
        {
            CoTaskMemFree(labelsArray[j].Key);
            CoTaskMemFree(labelsArray[j].Value);
        }
    });

    for (size_t i = 0; i < count; ++i)
    {
        const auto& label = std::next(m_labels.begin(), i);
        labelsArray[i].Key = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(label->first.c_str()).release();
        labelsArray[i].Value = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(label->second.c_str()).release();
    }

    cleanup.release();

    *Count = static_cast<ULONG>(count);
    *Labels = labelsArray.release();
}

HRESULT WSLAContainer::GetLabels(WSLA_LABEL_INFORMATION** Labels, ULONG* Count)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF(E_POINTER, Labels == nullptr || Count == nullptr);

    *Count = 0;
    *Labels = nullptr;
    return CallImpl(&WSLAContainerImpl::GetLabels, Labels, Count);
}
CATCH_RETURN();

HRESULT WSLAContainer::InterfaceSupportsErrorInfo(REFIID riid)
{
    return riid == __uuidof(IWSLAContainer) ? S_OK : S_FALSE;
}