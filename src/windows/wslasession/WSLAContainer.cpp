/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLAContainer.cpp

Abstract:

    Contains the implementation of WSLAContainer.
    N.B. This class is designed to allow multiple container operations to run in parallel.
    Operations that don't change the state of the container must be const qualified, and acquire a shared lock on m_lock.
    Operations that do change the container's state must acquire m_lock exclusively.
    Operations that interact with processes inside the container or the init process must acquire m_processesLock.
    m_lock must always be acquired before m_processesLock

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
using wsl::windows::service::wsla::ContainerPortMapping;
using wsl::windows::service::wsla::RelayedProcessIO;
using wsl::windows::service::wsla::VMPortMapping;
using wsl::windows::service::wsla::WSLAContainer;
using wsl::windows::service::wsla::WSLAContainerImpl;
using wsl::windows::service::wsla::WSLAContainerMetadata;
using wsl::windows::service::wsla::WSLAContainerMetadataV1;
using wsl::windows::service::wsla::WSLAPortMapping;
using wsl::windows::service::wsla::WSLASession;
using wsl::windows::service::wsla::WSLAVirtualMachine;
using wsl::windows::service::wsla::WSLAVolumeMount;

using namespace wsl::windows::common::relay;
using namespace wsl::windows::common::docker_schema;
using namespace std::chrono_literals;

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

    THROW_HR_IF_NULL_MSG(E_INVALIDARG, array.Values, "StringArray.Values is null with Count=%lu", array.Count);

    std::vector<std::string> result;
    result.reserve(array.Count);
    for (ULONG i = 0; i < array.Count; i += 1)
    {
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, array.Values[i], "StringArray.Values[%lu] is null", i);
        result.emplace_back(array.Values[i]);
    }

    return result;
}

// Builds port mapping list from container options and returns the network mode string.
std::pair<std::vector<ContainerPortMapping>, std::string> ProcessPortMappings(const WSLAContainerOptions& options, WSLAVirtualMachine& virtualMachine)
{
    WSLAContainerNetworkType networkType = options.ContainerNetwork.ContainerNetworkType;

    // Determine network mode string.
    std::string networkMode;
    if (networkType == WSLAContainerNetworkTypeBridged)
    {
        networkMode = "bridge";
    }
    else if (networkType == WSLAContainerNetworkTypeHost)
    {
        networkMode = "host";
    }
    else if (networkType == WSLAContainerNetworkTypeNone)
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
        options.PortsCount > 0 && networkType == WSLAContainerNetworkTypeNone,
        "Port mappings are not supported without networking");

    std::vector<ContainerPortMapping> ports;
    ports.reserve(options.PortsCount);

    for (ULONG i = 0; i < options.PortsCount; i++)
    {
        auto& entry = ports.emplace_back(VMPortMapping::FromWSLAPortMapping(options.Ports[i]), options.Ports[i].ContainerPort);

        // Only allocate port for bridged network. Host mode ports are allocated when the container starts.
        if (networkType == WSLAContainerNetworkTypeBridged)
        {
            entry.VmMapping.AssignVmPort(virtualMachine.AllocatePort(options.Ports[i].Family, options.Ports[i].Protocol));
        }
    }

    return {std::move(ports), std::move(networkMode)};
}

void UnmountVolumes(std::vector<WSLAVolumeMount>& volumes, WSLAVirtualMachine& parentVM)
{
    for (auto& volume : volumes)
    {
        if (volume.Mounted)
        {
            if (SUCCEEDED(LOG_IF_FAILED(parentVM.UnmountWindowsFolder(volume.ParentVMPath.c_str()))))
            {
                volume.Mounted = false;
            }
        }
    }
}

auto MountVolumes(std::vector<WSLAVolumeMount>& volumes, WSLAVirtualMachine& parentVM)
{
    auto errorCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&volumes, &parentVM]() { UnmountVolumes(volumes, parentVM); });

    for (auto& volume : volumes)
    {
        auto result = parentVM.MountWindowsFolder(volume.HostPath.c_str(), volume.ParentVMPath.c_str(), volume.ReadOnly);
        THROW_IF_FAILED_MSG(result, "Failed to mount %ls -> %hs", volume.HostPath.c_str(), volume.ParentVMPath.c_str());
        volume.Mounted = true;
    }

    return std::move(errorCleanup);
}

WSLAContainerState DockerStateToWSLAState(ContainerState state)
{
    // TODO: Handle other states like Paused, Restarting, etc.
    switch (state)
    {
    case ContainerState::Created:
        return WSLAContainerState::WslaContainerStateCreated;
    case ContainerState::Running:
        return WSLAContainerState::WslaContainerStateRunning;
    case ContainerState::Exited:
    case ContainerState::Dead:
        return WSLAContainerState::WslaContainerStateExited;
    case ContainerState::Removing:
        return WSLAContainerState::WslaContainerStateDeleted;
    default:
        return WSLAContainerState::WslaContainerStateInvalid;
    }
}

WSLAContainerNetworkType DockerNetworkModeToWSLANetworkType(const std::string& mode)
{
    if (mode == "bridge")
    {
        return WSLAContainerNetworkTypeBridged;
    }
    else if (mode == "host")
    {
        return WSLAContainerNetworkTypeHost;
    }
    else if (mode == "none")
    {
        return WSLAContainerNetworkTypeNone;
    }

    THROW_HR_MSG(E_INVALIDARG, "Invalid networking mode: %hs", mode.c_str());
}

std::uint64_t ParseDockerTimestamp(const std::string& timestamp)
{
    // Docker timestamps are UTC ISO 8601, e.g. "2026-03-05T10:30:00.123456789Z".
    std::chrono::sys_seconds utcSeconds;
    std::istringstream stream(timestamp);
    stream >> std::chrono::parse("%FT%H:%M:%S%Z", utcSeconds);
    THROW_HR_IF_MSG(E_INVALIDARG, stream.fail(), "Failed to parse timestamp '%hs'", timestamp.c_str());

    return static_cast<std::uint64_t>(utcSeconds.time_since_epoch().count());
}

std::string CleanContainerName(const std::string& name)
{
    // Docker container names have a leading '/', strip it.
    if (!name.empty() && name[0] == '/')
    {
        return name.substr(1);
    }

    return name;
}

std::string ExtractContainerName(const std::vector<std::string>& names, const std::string& id)
{
    if (names.empty())
    {
        return id;
    }

    return CleanContainerName(names[0]);
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

ContainerPortMapping::ContainerPortMapping(VMPortMapping&& VmMapping, uint16_t ContainerPort) :
    VmMapping(std::move(VmMapping)), ContainerPort(ContainerPort)
{
}

ContainerPortMapping::ContainerPortMapping(ContainerPortMapping&& Other) :
    VmMapping(std::move(Other.VmMapping)), ContainerPort(Other.ContainerPort)
{
}

ContainerPortMapping& ContainerPortMapping::operator=(ContainerPortMapping&& Other)
{
    if (this != &Other)
    {
        VmMapping = std::move(Other.VmMapping);
        ContainerPort = Other.ContainerPort;
    }
    return *this;
}

const char* ContainerPortMapping::ProtocolString() const
{
    if (VmMapping.Protocol == IPPROTO_TCP)
    {
        return "tcp";
    }
    else
    {
        WI_ASSERT(VmMapping.Protocol == IPPROTO_UDP);
        return "udp";
    }
}

WSLAPortMapping ContainerPortMapping::Serialize() const
{
    return WSLAPortMapping{
        .HostPort = VmMapping.HostPort(),
        .VmPort = VmMapping.VmPort ? VmMapping.VmPort->Port() : ContainerPort,
        .ContainerPort = ContainerPort,
        .Family = VmMapping.BindAddress.si_family,
        .Protocol = VmMapping.Protocol,
        .BindingAddress = VmMapping.BindingAddressString()};
}

WSLAContainerImpl::WSLAContainerImpl(
    WSLASession& wslaSession,
    WSLAVirtualMachine& virtualMachine,
    std::string&& Id,
    std::string&& Name,
    std::string&& Image,
    WSLAContainerNetworkType NetworkMode,
    std::vector<WSLAVolumeMount>&& volumes,
    std::vector<ContainerPortMapping>&& ports,
    std::map<std::string, std::string>&& labels,
    std::function<void(const WSLAContainerImpl*)>&& onDeleted,
    ContainerEventTracker& EventTracker,
    DockerHTTPClient& DockerClient,
    IORelay& Relay,
    WSLAContainerState InitialState,
    WSLAProcessFlags InitProcessFlags,
    WSLAContainerFlags ContainerFlags) :
    m_wslaSession(wslaSession),
    m_virtualMachine(virtualMachine),
    m_name(std::move(Name)),
    m_image(std::move(Image)),
    m_networkingMode(NetworkMode),
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

    // Snapshot and clear process references under the lock.
    // Callbacks are then invoked without holding m_lock.
    decltype(m_processes) processes;
    decltype(m_initProcessControl) initProcessControl = nullptr;

    {
        auto lock = m_lock.lock_exclusive();
        std::lock_guard processesLock{m_processesLock};
        initProcessControl = std::exchange(m_initProcessControl, nullptr);
        processes = std::exchange(m_processes, {});
    }

    if (initProcessControl)
    {
        initProcessControl->OnContainerReleased();
    }

    for (auto& process : processes)
    {
        process->OnContainerReleased();
    }

    m_containerEvents.Reset();

    auto lock = m_lock.lock_exclusive();
    ReleaseResources();
}

void WSLAContainerImpl::OnProcessReleased(DockerExecProcessControl* process) noexcept
{
    std::lock_guard processesLock{m_processesLock};

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

const std::vector<ContainerPortMapping>& WSLAContainerImpl::GetPorts() const noexcept
{
    return m_mappedPorts;
}

void WSLAContainerImpl::GetStateChangedAt(ULONGLONG* Result)
{
    auto lock = m_lock.lock_shared();
    *Result = m_stateChangedAt;
}

void WSLAContainerImpl::GetCreatedAt(ULONGLONG* Result)
{
    auto lock = m_lock.lock_shared();
    *Result = m_createdAt;
}

void WSLAContainerImpl::CopyTo(IWSLAContainer** Container) const
{
    auto lock = m_lock.lock_shared();

    THROW_HR_IF_MSG(RPC_E_DISCONNECTED, m_comWrapper == nullptr, "Container '%hs' is being released", m_id.c_str());

    THROW_IF_FAILED(m_comWrapper.CopyTo(Container));
}

void WSLAContainerImpl::Attach(LPCSTR DetachKeys, ULONG* Stdin, ULONG* Stdout, ULONG* Stderr) const
{
    auto lock = m_lock.lock_shared();

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        m_state != WslaContainerStateRunning,
        "Cannot attach to container '%hs', state: %i",
        m_id.c_str(),
        m_state);

    wil::unique_socket ioHandle;

    try
    {
        ioHandle = m_dockerClient.AttachContainer(m_id, DetachKeys == nullptr ? std::nullopt : std::optional<std::string>(DetachKeys));
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

void WSLAContainerImpl::Start(WSLAContainerStartFlags Flags, LPCSTR DetachKeys)
{
    // Acquire an exclusive lock since this method modifies m_initProcessControl, m_initProcess and m_state.
    auto lock = m_lock.lock_exclusive();

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        m_state != WslaContainerStateCreated && m_state != WslaContainerStateExited,
        "Cannot start container '%hs', state: %i",
        m_name.c_str(),
        m_state);

    // Attach to the container's init process so no IO is lost.
    std::unique_ptr<WSLAProcessIO> io;

    try
    {
        if (WI_IsFlagSet(Flags, WSLAContainerStartFlagsAttach))
        {
            auto detachKeys = DetachKeys == nullptr ? std::nullopt : std::optional<std::string>(DetachKeys);

            if (WI_IsFlagSet(m_initProcessFlags, WSLAProcessFlagsTty))
            {
                io = std::make_unique<TTYProcessIO>(wil::unique_handle{(HANDLE)m_dockerClient.AttachContainer(m_id, detachKeys).release()});
            }
            else
            {
                wil::unique_handle stream{reinterpret_cast<HANDLE>(m_dockerClient.AttachContainer(m_id, detachKeys).release())};
                io = CreateRelayedProcessIO(std::move(stream), m_initProcessFlags);
            }
        }
    }
    catch (const DockerHTTPException& e)
    {
        // N.B. This can happen if 'DetachKeys' is invalid.
        THROW_DOCKER_USER_ERROR_MSG(e, "Failed to attach to container '%hs' during start", m_id.c_str());
    }

    auto control = std::make_unique<DockerContainerProcessControl>(*this, m_dockerClient, m_eventTracker);

    std::lock_guard processesLock{m_processesLock};
    m_initProcessControl = control.get();

    m_initProcess = wil::MakeOrThrow<WSLAProcess>(std::move(control), std::move(io));

    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this]() mutable {
        m_initProcess.Reset();
        m_initProcessControl = nullptr;
    });

    m_stoppedNotifiedEvent.ResetEvent();

    auto volumeCleanup = MountVolumes(m_mountedVolumes, m_virtualMachine);

    auto portCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this]() { UnmapPorts(); });
    MapPorts();

    try
    {
        m_dockerClient.StartContainer(m_id, DetachKeys == nullptr ? std::nullopt : std::optional<std::string>(DetachKeys));
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to start container '%hs'", m_id.c_str());

    portCleanup.release();
    volumeCleanup.release();

    Transition(WslaContainerStateRunning);
    cleanup.release();
}

void WSLAContainerImpl::OnEvent(ContainerEvent event, std::optional<int> exitCode)
{
    if (event == ContainerEvent::Stop)
    {
        m_stoppedNotifiedEvent.SetEvent();

        THROW_HR_IF(E_UNEXPECTED, !exitCode.has_value());
        auto lock = m_lock.lock_exclusive();
        auto previousState = m_state;

        {
            std::lock_guard processesLock{m_processesLock};

            // Notify all processes that the container has exited.
            // N.B. The exec callback isn't always sent to execed processes, so do this to avoid 'stuck' processes.
            for (auto& process : m_processes)
            {
                process->OnContainerReleased();
            }

            m_processes.clear();
        }

        // Don't run the deletion logic if the container is already in a stopped / deleted state.
        // This can happen if Delete() is called by the user.
        if (previousState == WslaContainerStateRunning)
        {
            Transition(WslaContainerStateExited);

            ReleaseRuntimeResources();

            if (WI_IsFlagSet(m_containerFlags, WSLAContainerFlagsRm))
            {
                DeleteExclusiveLockHeld(WSLADeleteFlagsNone);
            }
        }
    }
    else if (event == ContainerEvent::Destroy)
    {
        auto lock = m_lock.lock_exclusive();
        if (m_state != WslaContainerStateDeleted)
        {
            Transition(WslaContainerStateDeleted);
        }
    }

    WSL_LOG(
        "ContainerEvent",
        TraceLoggingValue(m_name.c_str(), "Name"),
        TraceLoggingValue(m_id.c_str(), "Id"),
        TraceLoggingValue((int)event, "Event"));
}

void WSLAContainerImpl::Stop(WSLASignal Signal, LONG TimeoutSeconds)
{
    // Acquire an exclusive lock since this method modifies m_state.
    auto lock = m_lock.lock_exclusive();

    if (m_state == WslaContainerStateExited)
    {
        return;
    }
    else if (m_state != WslaContainerStateRunning)
    {
        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
            m_state != WslaContainerStateRunning,
            "Cannot stop container '%hs', state: %i",
            m_id.c_str(),
            m_state);
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

    Transition(WslaContainerStateExited);

    ReleaseRuntimeResources();

    if (WI_IsFlagSet(m_containerFlags, WSLAContainerFlagsRm))
    {
        DeleteExclusiveLockHeld(WSLADeleteFlagsForce);
    }
    else
    {
        // Wait for the stop notification to arrive before returning.
        // This is required so that a caller that Stops() and immediately calls Start() again doesn't see the container
        // switch back to 'stopped' state due to the delayed event notification.

        auto io = m_wslaSession.CreateIOContext();
        io.AddHandle(std::make_unique<EventHandle>(m_stoppedNotifiedEvent.get()), MultiHandleWait::CancelOnCompleted);

        io.Run({60s});
    }
}

void WSLAContainerImpl::Delete(WSLADeleteFlags Flags)
{
    // Acquire an exclusive lock since this method modifies m_state.
    auto lock = m_lock.lock_exclusive();

    DeleteExclusiveLockHeld(Flags);
}

__requires_exclusive_lock_held(m_lock) void WSLAContainerImpl::DeleteExclusiveLockHeld(WSLADeleteFlags Flags)
{
    // Validate that the container is not running or already deleted.
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        (m_state == WslaContainerStateRunning && WI_IsFlagClear(Flags, WSLADeleteFlagsForce)) || m_state == WslaContainerStateDeleted,
        "Cannot delete container '%hs', state: %i",
        m_name.c_str(),
        m_state);

    WI_ASSERT(m_state != WslaContainerStateInvalid);

    try
    {
        m_dockerClient.DeleteContainer(m_id, WI_IsFlagSet(Flags, WSLADeleteImageFlagsForce));
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to delete container '%hs'", m_id.c_str());

    ReleaseResources();

    Transition(WslaContainerStateDeleted);
}

void WSLAContainerImpl::Export(ULONG OutHandle) const
{
    auto lock = m_lock.lock_shared();

    // Validate that the container is not in the running state.
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        m_state == WslaContainerStateRunning,
        "Cannot export container '%hs', state: %i",
        m_name.c_str(),
        m_state);

    std::pair<uint32_t, wil::unique_socket> SocketCodePair;
    SocketCodePair = m_dockerClient.ExportContainer(m_id);

    wil::unique_handle containerFileHandle{wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(OutHandle))};

    wsl::windows::common::relay::MultiHandleWait io = m_wslaSession.CreateIOContext();

    std::string errorJson;
    auto accumulateError = [&](const gsl::span<char>& buffer) {
        // If the export failed, accumulate the error message.
        errorJson.append(buffer.data(), buffer.size());
    };

    if (SocketCodePair.first != 200)
    {
        io.AddHandle(std::make_unique<ReadHandle>(HandleWrapper{std::move(SocketCodePair.second)}, std::move(accumulateError)));
    }
    else
    {
        io.AddHandle(
            std::make_unique<RelayHandle<HTTPChunkBasedReadHandle>>(
                HandleWrapper{std::move(SocketCodePair.second)}, HandleWrapper{std::move(containerFileHandle)}),
            wsl::windows::common::relay::MultiHandleWait::CancelOnCompleted);
    }

    // Release the lock so the container can still be interacted with while the export is in progress.
    // Passed this point, no member variables can be accessed.
    lock.reset();

    io.Run({});

    if (SocketCodePair.first != 200)
    {
        // Export failed, parse the error message.
        auto error = wsl::shared::FromJson<common::docker_schema::ErrorResponse>(errorJson.c_str());

        THROW_HR_WITH_USER_ERROR_IF(WSLA_E_CONTAINER_NOT_FOUND, error.message, SocketCodePair.first == 404);
        THROW_HR_WITH_USER_ERROR(E_FAIL, error.message);
    }
}

void WSLAContainerImpl::GetState(WSLAContainerState* Result)
{
    auto lock = m_lock.lock_shared();
    *Result = m_state;
}

void WSLAContainerImpl::GetInitProcess(IWSLAProcess** Process) const
{
    auto lock = m_lock.lock_shared();
    std::lock_guard processesLock{m_processesLock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_initProcess);
    THROW_IF_FAILED(m_initProcess.CopyTo(__uuidof(IWSLAProcess), (void**)Process));
}

void WSLAContainerImpl::Exec(const WSLAProcessOptions* Options, LPCSTR DetachKeys, IWSLAProcess** Process)
{
    THROW_HR_IF_MSG(E_INVALIDARG, Options->CommandLine.Count == 0, "Exec command line cannot be empty");

    auto lock = m_lock.lock_shared();

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        m_state != WslaContainerStateRunning,
        "Container %hs is not running. State: %i",
        m_name.c_str(),
        m_state);

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

    if (DetachKeys != nullptr)
    {
        request.DetachKeys = DetachKeys;
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

        {
            std::lock_guard processesLock{m_processesLock};

            // Store a non owning reference to the process.
            m_processes.push_back(control.get());
        }

        // Poll for the exec'd process to either be running, or failed.
        // This is required because StartExec() returns before the process is actually created, and if exec() fails, we'll never
        // get an exec_die notification, so this case needs to be caught before returning the process to the caller.

        // TODO: Configurable timeout.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

        do
        {
            auto state = m_dockerClient.InspectExec(result.Id);
            if (state.Running && state.Pid.has_value())
            {
                control->SetPid(state.Pid.value());
                break; // Exec is running, exit.
            }
            else if (state.ExitCode.has_value())
            {
                control->SetExitCode(state.ExitCode.value());
                break; // Exec has exited, exit.
            }
            else if (std::chrono::steady_clock::now() > deadline)
            {
                THROW_HR_MSG(
                    HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                    "Timed out waiting for exec state for '%hs'. Last state: %hs",
                    result.Id.c_str(),
                    wsl::shared::ToJson(state).c_str());
            }

        } while (!control->GetExitEvent().wait(100));

        auto process = wil::MakeOrThrow<WSLAProcess>(std::move(control), std::move(io));
        THROW_IF_FAILED(process.CopyTo(__uuidof(IWSLAProcess), (void**)Process));
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to exec process in container %hs", m_id.c_str());
}

WslaInspectContainer WSLAContainerImpl::BuildInspectContainer(const DockerInspectContainer& dockerInspect) const
{
    WslaInspectContainer wslaInspect{};

    wslaInspect.Id = dockerInspect.Id;
    wslaInspect.Name = CleanContainerName(dockerInspect.Name);
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
        // TODO: ipv6 support.
        auto portKey = std::format("{}/{}", e.ContainerPort, e.ProtocolString());

        wsla_schema::InspectPortBinding portBinding{};
        portBinding.HostPort = std::to_string(e.VmMapping.HostPort());

        wslaInspect.Ports[portKey].push_back(std::move(portBinding));
    }

    // Map volume mounts using WSLA's host-side data.
    wslaInspect.Mounts.reserve(m_mountedVolumes.size() + dockerInspect.HostConfig.Tmpfs.size());
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

    // Map tmpfs mounts from Docker inspect data.
    for (const auto& entry : dockerInspect.HostConfig.Tmpfs)
    {
        wsla_schema::InspectMount mountInfo{};
        mountInfo.Type = "tmpfs";
        mountInfo.Destination = entry.first;
        // Tmpfs mounts are read-write by default. We currently do not parse tmpfs options
        // (e.g. "ro") for inspect output; Docker enforces actual mount behavior.
        mountInfo.ReadWrite = true;
        wslaInspect.Mounts.push_back(std::move(mountInfo));
    }

    return wslaInspect;
}

std::unique_ptr<WSLAContainerImpl> WSLAContainerImpl::Create(
    const WSLAContainerOptions& containerOptions,
    WSLASession& wslaSession,
    WSLAVirtualMachine& virtualMachine,
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

    if (containerOptions.DnsServers.Count > 0)
    {
        THROW_HR_IF_NULL_MSG(
            E_INVALIDARG,
            containerOptions.DnsServers.Values,
            "DnsServers.Values is null with Count=%lu",
            containerOptions.DnsServers.Count);

        request.HostConfig.Dns = StringArrayToVector(containerOptions.DnsServers);
    }

    if (containerOptions.DnsSearchDomains.Count > 0)
    {
        THROW_HR_IF_NULL_MSG(
            E_INVALIDARG,
            containerOptions.DnsSearchDomains.Values,
            "DnsSearchDomains.Values is null with Count=%lu",
            containerOptions.DnsSearchDomains.Count);

        request.HostConfig.DnsSearch = StringArrayToVector(containerOptions.DnsSearchDomains);
    }

    if (containerOptions.DnsOptions.Count > 0)
    {
        THROW_HR_IF_NULL_MSG(
            E_INVALIDARG,
            containerOptions.DnsOptions.Values,
            "DnsOptions.Values is null with Count=%lu",
            containerOptions.DnsOptions.Count);

        request.HostConfig.DnsOptions = StringArrayToVector(containerOptions.DnsOptions);
    }

    if (containerOptions.InitProcessOptions.User != nullptr)
    {
        request.User = containerOptions.InitProcessOptions.User;
    }

    request.HostConfig.Init = WI_IsFlagSet(containerOptions.Flags, WSLAContainerFlagsInit);

    if (containerOptions.VolumesCount > 0)
    {
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, containerOptions.Volumes, "Volumes is null with VolumesCount=%lu", containerOptions.VolumesCount);
    }

    // Build volume list from container options.
    std::vector<WSLAVolumeMount> volumes;
    volumes.reserve(containerOptions.VolumesCount);

    std::vector<std::string> binds;
    binds.reserve(containerOptions.VolumesCount);

    for (ULONG i = 0; i < containerOptions.VolumesCount; i++)
    {
        GUID volumeId;
        THROW_IF_FAILED(CoCreateGuid(&volumeId));

        auto parentVMPath = std::format("/mnt/{}", wsl::shared::string::GuidToString<char>(volumeId));
        auto volume = containerOptions.Volumes[i];

        THROW_HR_IF_NULL_MSG(E_INVALIDARG, volume.HostPath, "Volumes[%lu].HostPath is null", i);
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, volume.ContainerPath, "Volumes[%lu].ContainerPath is null", i);

        volumes.push_back(WSLAVolumeMount{volume.HostPath, parentVMPath, volume.ContainerPath, static_cast<bool>(volume.ReadOnly)});

        auto options = volume.ReadOnly ? "ro" : "rw";
        auto bind = std::format("{}:{}:{}", parentVMPath, volume.ContainerPath, options);

        binds.push_back(std::move(bind));
    }

    request.HostConfig.Binds = std::move(binds);

    // Process tmpfs mounts from container options.
    if (containerOptions.TmpfsCount > 0)
    {
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, containerOptions.Tmpfs, "Tmpfs is null with TmpfsCount=%lu", containerOptions.TmpfsCount);

        for (ULONG i = 0; i < containerOptions.TmpfsCount; i++)
        {
            const auto& tmpfs = containerOptions.Tmpfs[i];

            THROW_HR_IF_NULL_MSG(E_INVALIDARG, tmpfs.Destination, "Tmpfs mount at index %lu has null destination", i);

            request.HostConfig.Tmpfs[tmpfs.Destination] = tmpfs.Options != nullptr ? tmpfs.Options : "";
        }
    }

    // Process port mappings from container options.
    auto [ports, networkMode] = ProcessPortMappings(containerOptions, virtualMachine);
    request.HostConfig.NetworkMode = networkMode;

    for (const auto& e : ports)
    {
        auto portKey = std::format("{}/{}", e.ContainerPort, e.ProtocolString());
        request.ExposedPorts[portKey] = {};

        auto& portEntry = request.HostConfig.PortBindings[portKey];

        // In host mode, VmPort is empty until the container starts.
        // In that networking mode, the host port always matches the vm port.
        auto hostPort = e.VmMapping.VmPort ? e.VmMapping.VmPort->Port() : e.VmMapping.HostPort();

        portEntry.emplace_back(
            common::docker_schema::PortMapping{.HostIp = e.VmMapping.BindingAddressString(), .HostPort = std::to_string(hostPort)});
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

    for (const auto& e : ports)
    {
        metadata.Ports.emplace_back(e.Serialize());
    }

    request.Labels[WSLAContainerMetadataLabel] = SerializeContainerMetadata(metadata);
    request.Labels.insert(labels.begin(), labels.end());

    // Send the request to docker.
    auto result =
        DockerClient.CreateContainer(request, containerOptions.Name != nullptr ? containerOptions.Name : std::optional<std::string>{});

    // If no name was passed, inspect the container to fetch its generated name.
    common::docker_schema::InspectContainer inspectData;
    if (containerOptions.Name == nullptr)
    {
        inspectData = DockerClient.InspectContainer(result.Id);
    }

    auto container = std::make_unique<WSLAContainerImpl>(
        wslaSession,
        virtualMachine,
        std::move(result.Id),
        std::move(containerOptions.Name == nullptr ? CleanContainerName(inspectData.Name) : std::string(containerOptions.Name)),
        std::move(std::string(containerOptions.Image)),
        containerOptions.ContainerNetwork.ContainerNetworkType,
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

    return container;
}

std::unique_ptr<WSLAContainerImpl> WSLAContainerImpl::Open(
    const common::docker_schema::ContainerInfo& dockerContainer,
    WSLASession& wslaSession,
    WSLAVirtualMachine& virtualMachine,
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

    WI_ASSERT(dockerContainer.State != ContainerState::Running);

    auto metadata = ParseContainerMetadata(metadataIt->second.c_str());
    labels.erase(metadataIt);

    auto networkingMode = DockerNetworkModeToWSLANetworkType(dockerContainer.HostConfig.NetworkMode);
    // Re-register recovered VM ports in the allocation pool to prevent conflicts.
    std::vector<ContainerPortMapping> ports;
    for (const auto& e : metadata.Ports)
    {
        auto& inserted = ports.emplace_back(ContainerPortMapping{VMPortMapping::FromContainerMetaData(e), e.ContainerPort});

        if (networkingMode == WSLAContainerNetworkTypeBridged)
        {
            auto allocation = virtualMachine.TryAllocatePort(e.VmPort, e.Family, e.Protocol);

            THROW_HR_IF_MSG(
                HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
                !allocation,
                "Port %hu is in use, cannot open container %hs",
                e.VmPort,
                dockerContainer.Id.c_str());

            inserted.VmMapping.AssignVmPort(allocation);
        }
    }

    auto container = std::make_unique<WSLAContainerImpl>(
        wslaSession,
        virtualMachine,
        std::string(dockerContainer.Id),
        std::move(name),
        std::string(dockerContainer.Image),
        networkingMode,
        std::move(metadata.Volumes),
        std::move(ports),
        std::move(labels),
        std::move(OnDeleted),
        EventTracker,
        DockerClient,
        ioRelay,
        DockerStateToWSLAState(dockerContainer.State),
        metadata.InitProcessFlags,
        metadata.Flags);

    // Restore the creation timestamp from Docker list API data.
    container->m_createdAt = static_cast<std::uint64_t>(dockerContainer.Created);

    // Restore the state change timestamp from Docker inspect data.
    try
    {
        auto inspectData = DockerClient.InspectContainer(dockerContainer.Id);
        auto state = DockerStateToWSLAState(dockerContainer.State);
        const auto& timestamp = (state == WslaContainerStateRunning) ? inspectData.State.StartedAt : inspectData.State.FinishedAt;

        if (!timestamp.empty())
        {
            container->m_stateChangedAt = ParseDockerTimestamp(timestamp);
        }
    }
    CATCH_LOG();

    return container;
}

const std::string& WSLAContainerImpl::ID() const noexcept
{
    return m_id;
}

void WSLAContainerImpl::Inspect(LPSTR* Output) const
{
    auto lock = m_lock.lock_shared();

    try
    {
        // Get Docker inspect data
        auto dockerInspect = m_dockerClient.InspectContainer(m_id);

        // Convert to WSLA schema
        auto wslaInspect = BuildInspectContainer(dockerInspect);

        // Serialize WSLA schema to JSON
        std::string wslaJson = wsl::shared::ToJson(wslaInspect);
        *Output = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(wslaJson.c_str()).release();
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to inspect container '%hs'", m_id.c_str());
}

void WSLAContainerImpl::Logs(WSLALogsFlags Flags, ULONG* Stdout, ULONG* Stderr, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail) const
{
    auto lock = m_lock.lock_shared();

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

void WSLAContainerImpl::MapPorts()
{
    std::map<uint16_t, std::shared_ptr<VmPortAllocation>> allocatedPorts;

    for (auto& e : m_mappedPorts)
    {
        // VmPort is empty when the container is using host mode.
        // In that case, allocate the VM ports to match the container ports.
        if (!e.VmMapping.VmPort)
        {
            // Reuse existing vm port allocation when possible.
            // This is required because the same container can be bind the port number for different families or protocols.
            auto existing = allocatedPorts.find(e.ContainerPort);
            if (existing != allocatedPorts.end())
            {
                e.VmMapping.AssignVmPort(existing->second);
            }
            else
            {
                auto allocatedPort =
                    m_virtualMachine.TryAllocatePort(e.ContainerPort, e.VmMapping.BindAddress.si_family, e.VmMapping.Protocol);

                THROW_HR_IF_MSG(
                    HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
                    !allocatedPort,
                    "Port %hu is in use, cannot start container %hs",
                    e.ContainerPort,
                    m_id.c_str());

                e.VmMapping.AssignVmPort(allocatedPort);

                allocatedPorts.emplace(e.ContainerPort, allocatedPort);
            }
        }

        m_virtualMachine.MapPort(e.VmMapping);
    }
}

void WSLAContainerImpl::UnmapPorts()
{
    for (auto& e : m_mappedPorts)
    {
        try
        {
            e.VmMapping.Unmap();
        }
        CATCH_LOG();

        try
        {
            if (m_networkingMode == WSLAContainerNetworkTypeHost)
            {
                e.VmMapping.VmPort.reset();
            }
        }
        CATCH_LOG();
    }
}

__requires_exclusive_lock_held(m_lock) void WSLAContainerImpl::ReleaseRuntimeResources()
{
    WSL_LOG("ReleaseRuntimeResources", TraceLoggingValue(m_id.c_str(), "ID"));

    // Release runtime resources (port relays, volume mounts) that were set up at Start().
    UnmapPorts();
    UnmountVolumes(m_mountedVolumes, m_virtualMachine);
}

__requires_exclusive_lock_held(m_lock) void WSLAContainerImpl::ReleaseResources()
{
    WSL_LOG("ReleaseResources", TraceLoggingValue(m_id.c_str(), "ID"));

    ReleaseRuntimeResources();

    // Release VM port allocations back to the pool.
    for (auto& e : m_mappedPorts)
    {
        e.VmMapping.VmPort.reset();
    }

    // Disconnect the COM wrapper so no new RPC calls can reach this container.
    DisconnectComWrapper();
}

__requires_exclusive_lock_held(m_lock) void WSLAContainerImpl::DisconnectComWrapper()
{
    if (m_comWrapper)
    {
        m_comWrapper->Disconnect();
        m_comWrapper.Reset();
    }
}

__requires_lock_held(m_lock) void WSLAContainerImpl::Transition(WSLAContainerState State) noexcept
{
    // N.B. A deleted container cannot transition back to any other state.
    WI_ASSERT(m_state != WslaContainerStateDeleted);

    WSL_LOG(
        "ContainerStateChange",
        TraceLoggingValue(static_cast<int>(m_state), "PreviousState"),
        TraceLoggingValue(static_cast<int>(State), "NewState"),
        TraceLoggingValue(m_id.c_str(), "ID"));

    m_state = State;
    m_stateChangedAt = static_cast<std::uint64_t>(std::time(nullptr));
}

WSLAContainer::WSLAContainer(WSLAContainerImpl* impl, std::function<void(const WSLAContainerImpl*)>&& OnDeleted) :
    COMImplClass<WSLAContainerImpl>(impl), m_onDeleted(std::move(OnDeleted))
{
}

HRESULT WSLAContainer::Attach(LPCSTR DetachKeys, ULONG* Stdin, ULONG* Stdout, ULONG* Stderr)
{
    COMServiceExecutionContext context;

    *Stdin = 0;
    *Stdout = 0;
    *Stderr = 0;

    return CallImpl(&WSLAContainerImpl::Attach, DetachKeys, Stdin, Stdout, Stderr);
}

HRESULT WSLAContainer::GetState(WSLAContainerState* Result)
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

HRESULT WSLAContainer::Exec(const WSLAProcessOptions* Options, LPCSTR DetachKeys, IWSLAProcess** Process)
{
    COMServiceExecutionContext context;

    *Process = nullptr;
    return CallImpl(&WSLAContainerImpl::Exec, Options, DetachKeys, Process);
}

HRESULT WSLAContainer::Stop(_In_ WSLASignal Signal, _In_ LONG TimeoutSeconds)
{
    COMServiceExecutionContext context;

    return CallImpl(&WSLAContainerImpl::Stop, Signal, TimeoutSeconds);
}

HRESULT WSLAContainer::Start(WSLAContainerStartFlags Flags, LPCSTR DetachKeys)
{
    COMServiceExecutionContext context;

    return CallImpl(&WSLAContainerImpl::Start, Flags, DetachKeys);
}

HRESULT WSLAContainer::Inspect(LPSTR* Output)
{
    COMServiceExecutionContext context;

    *Output = nullptr;

    return CallImpl(&WSLAContainerImpl::Inspect, Output);
}

HRESULT WSLAContainer::Delete(WSLADeleteFlags Flags)
try
{
    COMServiceExecutionContext context;

    THROW_HR_IF_MSG(E_INVALIDARG, WI_IsAnyFlagSet(Flags, ~WSLADeleteFlagsValid), "Invalid flags: %i", Flags);

    // Special case for Delete(): If deletion is successful, notify the WSLASession that the container has been deleted.
    auto [lock, impl] = LockImpl();

    impl->Delete(Flags);
    m_onDeleted(impl);

    return S_OK;
}
CATCH_RETURN();

HRESULT WSLAContainer::Export(ULONG OutHandle)
{
    COMServiceExecutionContext context;

    return CallImpl(&WSLAContainerImpl::Export, OutHandle);
}

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

void WSLAContainerImpl::GetLabels(WSLALabelInformation** Labels, ULONG* Count) const
{
    auto lock = m_lock.lock_shared();

    if (m_labels.empty())
    {
        *Labels = nullptr;
        *Count = 0;
        return;
    }

    // Build labels locally using RAII strings. If an allocation throws mid-loop,
    // the vector destructor frees everything already built.
    std::vector<std::pair<wil::unique_cotaskmem_ansistring, wil::unique_cotaskmem_ansistring>> localLabels;
    localLabels.reserve(m_labels.size());

    for (const auto& [key, value] : m_labels)
    {
        localLabels.emplace_back(
            wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(key.c_str()),
            wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(value.c_str()));
    }

    // All strings built successfully — allocate output array and transfer ownership.
    auto labelsArray = wil::make_unique_cotaskmem<WSLALabelInformation[]>(localLabels.size());
    for (size_t i = 0; i < localLabels.size(); ++i)
    {
        labelsArray[i].Key = localLabels[i].first.release();
        labelsArray[i].Value = localLabels[i].second.release();
    }

    *Count = static_cast<ULONG>(localLabels.size());
    *Labels = labelsArray.release();
}

HRESULT WSLAContainer::GetLabels(WSLALabelInformation** Labels, ULONG* Count)
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