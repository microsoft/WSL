/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCContainer.cpp

Abstract:

    Contains the implementation of WSLCContainer.
    N.B. This class is designed to allow multiple container operations to run in parallel.
    Operations that don't change the state of the container must be const qualified, and acquire a shared lock on m_lock.
    Operations that do change the container's state must acquire m_lock exclusively.
    Operations that interact with processes inside the container or the init process must acquire m_processesLock.
    m_lock must always be acquired before m_processesLock

--*/

#include "precomp.h"
#include "WSLCContainer.h"
#include "WSLCProcess.h"
#include "WSLCProcessIO.h"

using wsl::windows::common::COMServiceExecutionContext;
using wsl::windows::common::docker_schema::ErrorResponse;
using wsl::windows::common::relay::DockerIORelayHandle;
using wsl::windows::common::relay::HandleWrapper;
using wsl::windows::common::relay::HTTPChunkBasedReadHandle;
using wsl::windows::common::relay::OverlappedIOHandle;
using wsl::windows::common::relay::ReadHandle;
using wsl::windows::common::relay::RelayHandle;
using wsl::windows::service::wslc::ContainerPortMapping;
using wsl::windows::service::wslc::IWSLCVolume;
using wsl::windows::service::wslc::RelayedProcessIO;
using wsl::windows::service::wslc::TypedHandle;
using wsl::windows::service::wslc::VMPortMapping;
using wsl::windows::service::wslc::WSLCContainer;
using wsl::windows::service::wslc::WSLCContainerImpl;
using wsl::windows::service::wslc::WSLCContainerMetadata;
using wsl::windows::service::wslc::WSLCContainerMetadataV1;
using wsl::windows::service::wslc::WSLCPortMapping;
using wsl::windows::service::wslc::WSLCSession;
using wsl::windows::service::wslc::WSLCVirtualMachine;
using wsl::windows::service::wslc::WSLCVolumeMount;

using namespace wsl::windows::common::relay;
using namespace wsl::windows::common::docker_schema;
using namespace wsl::windows::common::wslutil;
using namespace std::chrono_literals;
using wsl::shared::Localization;

namespace wslc_schema = wsl::windows::common::wslc_schema;

using DockerInspectContainer = wsl::windows::common::docker_schema::InspectContainer;
using WslcInspectContainer = wsl::windows::common::wslc_schema::InspectContainer;

namespace {

std::vector<std::string> StringArrayToVector(const WSLCStringArray& array)
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

// Parses a Docker ExposedPorts key (e.g. "8080/tcp", "5432/udp") into port number and protocol.
std::pair<uint16_t, int> ParseExposedPortKey(const std::string& key)
{
    auto slashPos = key.find('/');
    THROW_HR_IF_MSG(E_INVALIDARG, slashPos == std::string::npos, "Invalid exposed port format: %hs", key.c_str());

    auto portStr = std::string_view(key.c_str(), slashPos);

    uint16_t port{};
    auto result = std::from_chars(portStr.data(), portStr.data() + portStr.size(), port);
    if (result.ec != std::errc{} || result.ptr != portStr.data() + portStr.size() || port == 0)
    {
        THROW_HR_MSG(E_INVALIDARG, "Invalid port number in exposed port: %hs", key.c_str());
    }

    auto protoStr = key.substr(slashPos + 1);
    int protocol{};
    if (protoStr == "tcp")
    {
        protocol = IPPROTO_TCP;
    }
    else if (protoStr == "udp")
    {
        protocol = IPPROTO_UDP;
    }
    else
    {
        THROW_HR_MSG(E_INVALIDARG, "Unsupported protocol in exposed port: %hs", key.c_str());
    }

    return {static_cast<uint16_t>(port), protocol};
}

// Temporary solution to allocate an ephemeral port.
// TODO: Remove once the port relay can allocate ephemeral ports.
uint16_t AllocateEphemeralPort(int family, const char* address)
{
    wil::unique_socket sock(socket(family, SOCK_STREAM, IPPROTO_TCP));
    THROW_LAST_ERROR_IF(!sock);

    SOCKADDR_INET addr{};
    addr.si_family = static_cast<ADDRESS_FAMILY>(family);

    if (family == AF_INET)
    {
        THROW_HR_IF_MSG(E_INVALIDARG, inet_pton(AF_INET, address, &addr.Ipv4.sin_addr) != 1, "Failed to parse ip address: %hs", address);
    }
    else if (family == AF_INET6)
    {
        THROW_HR_IF_MSG(E_INVALIDARG, inet_pton(AF_INET6, address, &addr.Ipv6.sin6_addr) != 1, "Failed to parse ip address: %hs", address);
    }
    else
    {
        THROW_HR_MSG(E_UNEXPECTED, "Unexpected address family: %i", family);
    }

    THROW_LAST_ERROR_IF(bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR);

    int addrLen = sizeof(addr);
    THROW_LAST_ERROR_IF(getsockname(sock.get(), reinterpret_cast<sockaddr*>(&addr), &addrLen) == SOCKET_ERROR);

    uint16_t port = (family == AF_INET6) ? ntohs(addr.Ipv6.sin6_port) : ntohs(addr.Ipv4.sin_port);
    THROW_HR_IF_MSG(E_UNEXPECTED, port == 0, "OS returned ephemeral port 0");

    return port;
}

// Builds port mapping list from container options and returns the network mode string.
std::pair<std::vector<ContainerPortMapping>, std::string> ProcessPortMappings(
    std::vector<_WSLCPortMapping>& requestedPorts, WSLCContainerNetworkType networkType, WSLCVirtualMachine& virtualMachine)
{
    // Determine network mode string.
    std::string networkMode;
    if (networkType == WSLCContainerNetworkTypeBridged)
    {
        networkMode = "bridge";
    }
    else if (networkType == WSLCContainerNetworkTypeHost)
    {
        networkMode = "host";
    }
    else if (networkType == WSLCContainerNetworkTypeNone)
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
        !requestedPorts.empty() && networkType == WSLCContainerNetworkTypeNone,
        "Port mappings are not supported without networking");

    std::vector<ContainerPortMapping> ports;
    ports.reserve(requestedPorts.size());

    for (auto& e : requestedPorts)
    {
        if (e.HostPort == WSLC_EPHEMERAL_PORT)
        {
            e.HostPort = AllocateEphemeralPort(e.Family, e.BindingAddress);
        }

        auto& entry = ports.emplace_back(VMPortMapping::FromWSLCPortMapping(e), e.ContainerPort);

        // Only allocate port for bridged network. Host mode ports are allocated when the container starts.
        if (networkType == WSLCContainerNetworkTypeBridged)
        {
            entry.VmMapping.AssignVmPort(virtualMachine.AllocatePort(e.Family, e.Protocol));
        }
    }

    return {std::move(ports), std::move(networkMode)};
}

void UnmountVolumes(std::vector<WSLCVolumeMount>& volumes, WSLCVirtualMachine& parentVM)
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

auto MountVolumes(std::vector<WSLCVolumeMount>& volumes, WSLCVirtualMachine& parentVM)
{
    auto errorCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&volumes, &parentVM]() { UnmountVolumes(volumes, parentVM); });

    for (auto& volume : volumes)
    {
        // Create a new directory if it doesn't exist.
        if (!std::filesystem::exists(volume.HostPath))
        {
            auto result = wil::CreateDirectoryDeepNoThrow(volume.HostPath.c_str());
            if (FAILED(result))
            {
                THROW_HR_WITH_USER_ERROR(
                    result, Localization::MessageWslcFailedToMountVolume(volume.HostPath, wsl::windows::common::wslutil::GetErrorString(result)));
            }
        }

        auto result = parentVM.MountWindowsFolder(volume.HostPath.c_str(), volume.ParentVMPath.c_str(), volume.ReadOnly);
        THROW_IF_FAILED_MSG(result, "Failed to mount %ls -> %hs", volume.HostPath.c_str(), volume.ParentVMPath.c_str());
        volume.Mounted = true;
    }

    return std::move(errorCleanup);
}

WSLCContainerState DockerStateToWSLCState(ContainerState state)
{
    // TODO: Handle other states like Paused, Restarting, etc.
    switch (state)
    {
    case ContainerState::Created:
        return WSLCContainerState::WslcContainerStateCreated;
    case ContainerState::Running:
        return WSLCContainerState::WslcContainerStateRunning;
    case ContainerState::Exited:
    case ContainerState::Dead:
        return WSLCContainerState::WslcContainerStateExited;
    case ContainerState::Removing:
        return WSLCContainerState::WslcContainerStateDeleted;
    default:
        return WSLCContainerState::WslcContainerStateInvalid;
    }
}

WSLCContainerNetworkType DockerNetworkModeToWSLCNetworkType(const std::string& mode)
{
    if (mode == "bridge")
    {
        return WSLCContainerNetworkTypeBridged;
    }
    else if (mode == "host")
    {
        return WSLCContainerNetworkTypeHost;
    }
    else if (mode == "none")
    {
        return WSLCContainerNetworkTypeNone;
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

std::string FormatPortEndpoint(const ContainerPortMapping& portMapping)
{
    auto addr = portMapping.VmMapping.BindingAddressString();
    return std::format(
        "{}:{}/{}",
        portMapping.VmMapping.IsIPv6() ? std::format("[{}]", addr) : addr,
        portMapping.VmMapping.HostPort(),
        portMapping.ProtocolString());
}

WSLCContainerMetadataV1 ParseContainerMetadata(const std::string& json)
{
    auto wrapper = wsl::shared::FromJson<WSLCContainerMetadata>(json.c_str());
    THROW_HR_IF(E_UNEXPECTED, !wrapper.V1.has_value());

    return wrapper.V1.value();
}

std::string SerializeContainerMetadata(const WSLCContainerMetadataV1& metadata)
{
    WSLCContainerMetadata wrapper;
    wrapper.V1 = metadata;

    return wsl::shared::ToJson(wrapper);
}

void ProcessNamedVolumes(
    const WSLCContainerOptions& containerOptions,
    const std::unordered_map<std::string, std::unique_ptr<IWSLCVolume>>& sessionVolumes,
    wsl::windows::common::docker_schema::CreateContainer& request)
{
    THROW_HR_IF(E_INVALIDARG, containerOptions.NamedVolumesCount > 0 && containerOptions.NamedVolumes == nullptr);

    for (ULONG i = 0; i < containerOptions.NamedVolumesCount; i++)
    {
        const auto& nv = containerOptions.NamedVolumes[i];
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, nv.Name, "NamedVolume at index %lu has null Name", i);
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, nv.ContainerPath, "NamedVolume at index %lu has null ContainerPath", i);

        std::string volumeName = nv.Name;

        THROW_HR_WITH_USER_ERROR_IF(
            WSLC_E_VOLUME_NOT_FOUND, Localization::MessageWslcVolumeNotFound(nv.Name), !sessionVolumes.contains(volumeName));

        wsl::windows::common::docker_schema::Mount mount{};
        mount.Source = std::move(volumeName);
        mount.Target = std::string(nv.ContainerPath);
        mount.Type = "volume";
        mount.ReadOnly = static_cast<bool>(nv.ReadOnly);

        request.HostConfig.Mounts.emplace_back(mount);
    }
}

void ValidateNamedVolumes(
    const std::vector<wsl::windows::common::docker_schema::Mount>& mounts,
    const std::unordered_map<std::string, std::unique_ptr<IWSLCVolume>>& sessionVolumes,
    const std::unordered_set<std::string>& anonymousVolumes)
{
    for (const auto& mount : mounts)
    {
        if (mount.Type == "volume" && !mount.Name.empty())
        {
            THROW_HR_WITH_USER_ERROR_IF(
                WSLC_E_VOLUME_NOT_FOUND,
                Localization::MessageWslcVolumeNotFound(mount.Name),
                !sessionVolumes.contains(mount.Name) && !anonymousVolumes.contains(mount.Name));
        }
    }
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

WSLCPortMapping ContainerPortMapping::Serialize() const
{
    return WSLCPortMapping{
        .HostPort = VmMapping.HostPort(),
        .VmPort = VmMapping.VmPort ? VmMapping.VmPort->Port() : ContainerPort,
        .ContainerPort = ContainerPort,
        .Family = VmMapping.BindAddress.si_family,
        .Protocol = VmMapping.Protocol,
        .BindingAddress = VmMapping.BindingAddressString()};
}

WSLCContainerImpl::WSLCContainerImpl(
    WSLCSession& wslcSession,
    WSLCVirtualMachine& virtualMachine,
    std::string&& Id,
    std::string&& Name,
    std::string&& Image,
    WSLCContainerNetworkType NetworkMode,
    std::vector<WSLCVolumeMount>&& volumes,
    std::vector<ContainerPortMapping>&& ports,
    std::map<std::string, std::string>&& labels,
    std::function<void(const WSLCContainerImpl*)>&& onDeleted,
    ContainerEventTracker& EventTracker,
    DockerHTTPClient& DockerClient,
    IORelay& Relay,
    WSLCContainerState InitialState,
    std::uint64_t CreatedAt,
    WSLCProcessFlags InitProcessFlags,
    WSLCContainerFlags ContainerFlags) :
    m_wslcSession(wslcSession),
    m_virtualMachine(virtualMachine),
    m_name(std::move(Name)),
    m_image(std::move(Image)),
    m_networkingMode(NetworkMode),
    m_id(std::move(Id)),
    m_mountedVolumes(std::move(volumes)),
    m_mappedPorts(std::move(ports)),
    m_labels(std::move(labels)),
    m_comWrapper(wil::MakeOrThrow<WSLCContainer>(this, std::move(onDeleted))),
    m_dockerClient(DockerClient),
    m_eventTracker(EventTracker),
    m_ioRelay(Relay),
    m_containerEvents(EventTracker.RegisterContainerStateUpdates(
        m_id, std::bind(&WSLCContainerImpl::OnEvent, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3))),
    m_state(InitialState),
    m_createdAt(CreatedAt),
    m_initProcessFlags(InitProcessFlags),
    m_containerFlags(ContainerFlags)
{
}

WSLCContainerImpl::~WSLCContainerImpl()
{
    WSL_LOG(
        "~WSLCContainerImpl",
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

void WSLCContainerImpl::OnProcessReleased(DockerExecProcessControl* process) noexcept
{
    std::lock_guard processesLock{m_processesLock};

    auto remove = std::ranges::remove_if(m_processes, [process](const auto* e) { return e == process; });
    WI_ASSERT(remove.size() == 1);

    m_processes.erase(remove.begin(), remove.end());
}

const std::string& WSLCContainerImpl::Image() const noexcept
{
    return m_image;
}

const std::string& WSLCContainerImpl::Name() const noexcept
{
    return m_name;
}

std::vector<WSLCPortMapping> WSLCContainerImpl::GetPorts() const
{
    auto lock = m_lock.lock_shared();
    if (m_state != WslcContainerStateRunning)
    {
        return {};
    }

    std::vector<WSLCPortMapping> result;
    result.reserve(m_mappedPorts.size());
    for (const auto& port : m_mappedPorts)
    {
        result.push_back(port.Serialize());
    }
    return result;
}

void WSLCContainerImpl::GetStateChangedAt(ULONGLONG* Result)
{
    auto lock = m_lock.lock_shared();
    *Result = m_stateChangedAt;
}

void WSLCContainerImpl::GetCreatedAt(ULONGLONG* Result)
{
    auto lock = m_lock.lock_shared();
    *Result = m_createdAt;
}

void WSLCContainerImpl::CopyTo(IWSLCContainer** Container) const
{
    auto lock = m_lock.lock_shared();

    THROW_HR_IF_MSG(RPC_E_DISCONNECTED, m_comWrapper == nullptr, "Container '%hs' is being released", m_id.c_str());

    THROW_IF_FAILED(m_comWrapper.CopyTo(Container));
}

void WSLCContainerImpl::Attach(LPCSTR DetachKeys, WSLCHandle* Stdin, WSLCHandle* Stdout, WSLCHandle* Stderr) const
{
    auto lock = m_lock.lock_shared();

    THROW_HR_WITH_USER_ERROR_IF(WSLC_E_CONTAINER_NOT_RUNNING, Localization::MessageWslcContainerNotRunning(m_id.c_str()), m_state != WslcContainerStateRunning);

    wil::unique_socket ioHandle;

    try
    {
        ioHandle = m_dockerClient.AttachContainer(m_id, DetachKeys == nullptr ? std::nullopt : std::optional<std::string>(DetachKeys));
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to attach to container '%hs'", m_id.c_str());

    // If this is a TTY process, the PTY handle can be returned directly.
    if (WI_IsFlagSet(m_initProcessFlags, WSLCProcessFlagsTty))
    {
        *Stdin = common::wslutil::ToCOMOutputHandle(
            reinterpret_cast<HANDLE>(ioHandle.get()), GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, WSLCHandleTypeSocket);

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

    *Stdin = common::wslutil::ToCOMOutputHandle(reinterpret_cast<HANDLE>(stdinWrite.get()), GENERIC_WRITE | SYNCHRONIZE, WSLCHandleTypePipe);

    *Stdout = common::wslutil::ToCOMOutputHandle(reinterpret_cast<HANDLE>(stdoutRead.get()), GENERIC_READ | SYNCHRONIZE, WSLCHandleTypePipe);

    *Stderr = common::wslutil::ToCOMOutputHandle(reinterpret_cast<HANDLE>(stderrRead.get()), GENERIC_READ | SYNCHRONIZE, WSLCHandleTypePipe);
}

void WSLCContainerImpl::Start(WSLCContainerStartFlags Flags, LPCSTR DetachKeys)
{
    // Acquire an exclusive lock since this method modifies m_initProcessControl, m_initProcess and m_state.
    auto lock = m_lock.lock_exclusive();

    THROW_HR_WITH_USER_ERROR_IF(WSLC_E_CONTAINER_IS_RUNNING, Localization::MessageWslcContainerIsRunning(m_id), m_state == WslcContainerStateRunning);

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        m_state != WslcContainerStateCreated && m_state != WslcContainerStateExited,
        "Cannot start container '%hs', state %i",
        m_id.c_str(),
        m_state);

    // Attach to the container's init process so no IO is lost.
    std::unique_ptr<WSLCProcessIO> io;

    try
    {
        if (WI_IsFlagSet(Flags, WSLCContainerStartFlagsAttach))
        {
            auto detachKeys = DetachKeys == nullptr ? std::nullopt : std::optional<std::string>(DetachKeys);

            if (WI_IsFlagSet(m_initProcessFlags, WSLCProcessFlagsTty))
            {
                io = std::make_unique<TTYProcessIO>(TypedHandle{
                    wil::unique_handle{(HANDLE)m_dockerClient.AttachContainer(m_id, detachKeys).release()}, WSLCHandleTypeSocket});
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

    m_initProcess = wil::MakeOrThrow<WSLCProcess>(std::move(control), std::move(io), m_initProcessFlags);

    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this]() mutable {
        m_initProcess.Reset();
        m_initProcessControl = nullptr;
    });

    auto volumeCleanup = MountVolumes(m_mountedVolumes, m_virtualMachine);

    auto portCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this]() { UnmapPorts(); });
    MapPorts();

    m_stopNotification.Event.ResetEvent();
    m_stopNotification.EventTime.store(0, std::memory_order_relaxed);

    try
    {
        m_dockerClient.StartContainer(m_id, DetachKeys == nullptr ? std::nullopt : std::optional<std::string>(DetachKeys));
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to start container '%hs'", m_id.c_str());

    portCleanup.release();
    volumeCleanup.release();

    Transition(WslcContainerStateRunning);
    cleanup.release();
}

void WSLCContainerImpl::OnEvent(ContainerEvent event, std::optional<int> exitCode, std::uint64_t eventTime)
{
    if (event == ContainerEvent::Stop)
    {
        THROW_HR_IF(E_UNEXPECTED, !exitCode.has_value());

        m_stopNotification.EventTime.store(eventTime, std::memory_order_release);
        m_stopNotification.Event.SetEvent();

        auto lock = m_lock.lock_exclusive();
        auto previousState = m_state;

        ReleaseProcesses();

        // Don't run the deletion logic if the container is already in a stopped / deleted state.
        // This can happen if Delete() is called by the user.
        if (previousState == WslcContainerStateRunning)
        {
            Transition(WslcContainerStateExited, eventTime);

            ReleaseRuntimeResources();

            if (WI_IsFlagSet(m_containerFlags, WSLCContainerFlagsRm))
            {
                DeleteExclusiveLockHeld(WSLCDeleteFlagsDeleteVolumes);
            }
        }
    }
    else if (event == ContainerEvent::Destroy)
    {
        auto lock = m_lock.lock_exclusive();
        if (m_state != WslcContainerStateDeleted)
        {
            Transition(WslcContainerStateDeleted);
        }
    }

    WSL_LOG(
        "ContainerEvent",
        TraceLoggingValue(m_name.c_str(), "Name"),
        TraceLoggingValue(m_id.c_str(), "Id"),
        TraceLoggingValue((int)event, "Event"));
}

bool WSLCContainerImpl::WaitForEvent(const wil::unique_event& Event, std::chrono::milliseconds Timeout) const
{
    const HANDLE waitHandles[] = {Event.get(), m_wslcSession.SessionTerminatingEvent()};
    const DWORD waitResult = WaitForMultipleObjects(RTL_NUMBER_OF(waitHandles), waitHandles, FALSE, gsl::narrow<DWORD>(Timeout.count()));

    switch (waitResult)
    {
    case WAIT_OBJECT_0:
        return true;
    case WAIT_OBJECT_0 + 1:
        THROW_HR_MSG(E_ABORT, "Session %lu is terminating.", m_wslcSession.Id());
    case WAIT_TIMEOUT:
        return false;
    default:
        THROW_LAST_ERROR();
    }
}

void WSLCContainerImpl::Stop(WSLCSignal Signal, LONG TimeoutSeconds, bool Kill)
{
    // Acquire an exclusive lock since this method modifies m_state.
    auto lock = m_lock.lock_exclusive();

    if (m_state == WslcContainerStateExited && !Kill)
    {
        return;
    }
    else if (m_state != WslcContainerStateRunning)
    {
        THROW_HR_WITH_USER_ERROR_MSG(
            WSLC_E_CONTAINER_NOT_RUNNING,
            Localization::MessageWslcContainerNotRunning(m_id),
            "Cannot stop container '%hs', state: %i",
            m_id.c_str(),
            m_state);
    }

    std::optional<WSLCSignal> SignalArg;
    if (Signal != WSLCSignalNone)
    {
        SignalArg = Signal;
    }

    // Don't wait for the container to stop if we're not sending SIGKILL, since it may not stop the container.
    // N.B. If the signal was SIGTERM for instance, we'll receive the stop notification via OnEvent().
    bool waitForStop = !Kill || (SignalArg.value_or(WSLCSignalSIGKILL) == WSLCSignalSIGKILL);

    try
    {
        if (Kill)
        {
            m_dockerClient.SignalContainer(m_id, SignalArg);

            if (!waitForStop)
            {
                return;
            }
        }
        else
        {
            std::optional<ULONG> TimeoutArg;
            if (TimeoutSeconds >= 0)
            {
                TimeoutArg = static_cast<ULONG>(TimeoutSeconds);
            }

            m_dockerClient.StopContainer(m_id, SignalArg, TimeoutArg);
        }
    }
    catch (const DockerHTTPException& e)
    {
        // HTTP 304 is returned when the container is already stopped.
        if (Kill || e.StatusCode() != 304)
        {
            THROW_DOCKER_USER_ERROR_MSG(e, "Failed to %hs container '%hs'", Kill ? "kill" : "stop", m_id.c_str());
        }
    }

    // Wait for the stop event to get the Docker timestamp.
    // OnEvent() signals the event before taking m_lock, so this won't deadlock.
    std::optional<std::uint64_t> stopTimestamp;
    if (waitForStop && WaitForEvent(m_stopNotification.Event, 60s))
    {
        stopTimestamp = m_stopNotification.EventTime.load(std::memory_order_acquire);
    }

    Transition(WslcContainerStateExited, stopTimestamp);

    ReleaseProcesses();

    ReleaseRuntimeResources();

    if (WI_IsFlagSet(m_containerFlags, WSLCContainerFlagsRm))
    {
        DeleteExclusiveLockHeld(WSLCDeleteFlagsForce | WSLCDeleteFlagsDeleteVolumes);
    }
}

void WSLCContainerImpl::Delete(WSLCDeleteFlags Flags)
{
    // Acquire an exclusive lock since this method modifies m_state.
    auto lock = m_lock.lock_exclusive();

    DeleteExclusiveLockHeld(Flags);
}

__requires_exclusive_lock_held(m_lock) void WSLCContainerImpl::DeleteExclusiveLockHeld(WSLCDeleteFlags Flags)
{
    // Validate that the container is not running or already deleted.
    THROW_HR_WITH_USER_ERROR_IF(
        WSLC_E_CONTAINER_IS_RUNNING,
        Localization::MessageWslcCannotRemoveRunningContainer(m_id),
        m_state == WslcContainerStateRunning && WI_IsFlagClear(Flags, WSLCDeleteFlagsForce));

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE), m_state == WslcContainerStateDeleted, "Container %hs is already deleted", m_id.c_str());

    WI_ASSERT(m_state != WslcContainerStateInvalid);

    try
    {
        m_dockerClient.DeleteContainer(m_id, WI_IsFlagSet(Flags, WSLCDeleteFlagsForce), WI_IsFlagSet(Flags, WSLCDeleteFlagsDeleteVolumes));
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to delete container '%hs'", m_id.c_str());

    Transition(WslcContainerStateDeleted);
    ReleaseResources();
}

void WSLCContainerImpl::Export(WSLCHandle OutHandle) const
{
    auto lock = m_lock.lock_shared();

    // Validate that the container is not in the running state.
    THROW_HR_WITH_USER_ERROR_IF(WSLC_E_CONTAINER_IS_RUNNING, Localization::MessageWslcContainerIsRunning(m_id), m_state == WslcContainerStateRunning);

    std::pair<uint32_t, wil::unique_socket> SocketCodePair;
    SocketCodePair = m_dockerClient.ExportContainer(m_id);

    auto userHandle = m_wslcSession.OpenUserHandle(OutHandle);

    wsl::windows::common::relay::MultiHandleWait io = m_wslcSession.CreateIOContext();

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
            std::make_unique<RelayHandle<HTTPChunkBasedReadHandle>>(HandleWrapper{std::move(SocketCodePair.second)}, userHandle.Get()),
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

        THROW_HR_WITH_USER_ERROR_IF(WSLC_E_CONTAINER_NOT_FOUND, error.message, SocketCodePair.first == 404);
        THROW_HR_WITH_USER_ERROR(E_FAIL, error.message);
    }
}

void WSLCContainerImpl::GetState(WSLCContainerState* Result)
{
    auto lock = m_lock.lock_shared();
    *Result = m_state;
}

WSLCContainerState WSLCContainerImpl::State() const noexcept
{
    auto lock = m_lock.lock_shared();
    return m_state;
}

void WSLCContainerImpl::GetInitProcess(IWSLCProcess** Process) const
{
    auto lock = m_lock.lock_shared();
    std::lock_guard processesLock{m_processesLock};

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !m_initProcess);
    THROW_IF_FAILED(m_initProcess.CopyTo(__uuidof(IWSLCProcess), (void**)Process));
}

void WSLCContainerImpl::Exec(const WSLCProcessOptions* Options, LPCSTR DetachKeys, IWSLCProcess** Process)
{
    THROW_HR_IF_MSG(E_INVALIDARG, Options->CommandLine.Count == 0, "Exec command line cannot be empty");

    auto lock = m_lock.lock_shared();

    THROW_HR_WITH_USER_ERROR_IF(WSLC_E_CONTAINER_NOT_RUNNING, Localization::MessageWslcContainerNotRunning(m_id), m_state != WslcContainerStateRunning);

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

    if (WI_IsFlagSet(Options->Flags, WSLCProcessFlagsTty))
    {
        request.Tty = true;
    }

    if (WI_IsFlagSet(Options->Flags, WSLCProcessFlagsStdin))
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

        std::unique_ptr<WSLCProcessIO> io;
        if (request.Tty)
        {
            io = std::make_unique<TTYProcessIO>(TypedHandle{std::move(stream), WSLCHandleTypeSocket});
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

        auto process = wil::MakeOrThrow<WSLCProcess>(std::move(control), std::move(io), Options->Flags);
        THROW_IF_FAILED(process.CopyTo(__uuidof(IWSLCProcess), (void**)Process));
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to exec process in container %hs", m_id.c_str());
}

WslcInspectContainer WSLCContainerImpl::BuildInspectContainer(const DockerInspectContainer& dockerInspect) const
{
    WslcInspectContainer wslcInspect{};

    wslcInspect.Id = dockerInspect.Id;
    wslcInspect.Name = CleanContainerName(dockerInspect.Name);
    wslcInspect.Created = dockerInspect.Created;
    wslcInspect.Image = m_image;

    // Map container state.
    wslcInspect.State.Status = dockerInspect.State.Status;
    wslcInspect.State.Running = dockerInspect.State.Running;
    wslcInspect.State.ExitCode = dockerInspect.State.ExitCode;
    wslcInspect.State.StartedAt = dockerInspect.State.StartedAt;
    wslcInspect.State.FinishedAt = dockerInspect.State.FinishedAt;

    wslcInspect.HostConfig.NetworkMode = dockerInspect.HostConfig.NetworkMode;

    // Map WSLC port mappings (Windows host ports only). HostIp is not set here and will use
    // the default value ("127.0.0.1") defined in the InspectPortBinding schema.
    for (const auto& e : m_mappedPorts)
    {
        // TODO: ipv6 support.
        auto portKey = std::format("{}/{}", e.ContainerPort, e.ProtocolString());

        wslc_schema::InspectPortBinding portBinding{};
        portBinding.HostPort = std::to_string(e.VmMapping.HostPort());

        wslcInspect.Ports[portKey].push_back(std::move(portBinding));
    }

    // Map volume mounts using WSLC's host-side data.
    wslcInspect.Mounts.reserve(m_mountedVolumes.size() + dockerInspect.HostConfig.Tmpfs.size());
    for (const auto& volume : m_mountedVolumes)
    {
        wslc_schema::InspectMount mountInfo{};
        // TODO: Support different mount types (plan9/VHD) when VHD volumes are implemented.
        mountInfo.Type = "bind";

        // For file mounts, reconstruct the original host path from the parent directory and filename.
        if (volume.SourceFilename.empty())
        {
            mountInfo.Source = wsl::shared::string::WideToMultiByte(volume.HostPath);
        }
        else
        {
            std::filesystem::path fullPath(volume.HostPath);
            fullPath /= volume.SourceFilename;
            mountInfo.Source = fullPath.string();
        }

        mountInfo.Destination = volume.ContainerPath;
        mountInfo.ReadWrite = !volume.ReadOnly;
        wslcInspect.Mounts.push_back(std::move(mountInfo));
    }

    // Map tmpfs mounts from Docker inspect data.
    for (const auto& entry : dockerInspect.HostConfig.Tmpfs)
    {
        wslc_schema::InspectMount mountInfo{};
        mountInfo.Type = "tmpfs";
        mountInfo.Destination = entry.first;
        // Tmpfs mounts are read-write by default. We currently do not parse tmpfs options
        // (e.g. "ro") for inspect output; Docker enforces actual mount behavior.
        mountInfo.ReadWrite = true;
        wslcInspect.Mounts.push_back(std::move(mountInfo));
    }

    return wslcInspect;
}

std::unique_ptr<WSLCContainerImpl> WSLCContainerImpl::Create(
    const WSLCContainerOptions& containerOptions,
    WSLCSession& wslcSession,
    WSLCVirtualMachine& virtualMachine,
    const std::unordered_map<std::string, std::unique_ptr<IWSLCVolume>>& sessionVolumes,
    std::function<void(const WSLCContainerImpl*)>&& OnDeleted,
    ContainerEventTracker& EventTracker,
    DockerHTTPClient& DockerClient,
    IORelay& IoRelay)
{
    common::docker_schema::CreateContainer request;
    request.Image = containerOptions.Image;

    // TODO: Think about when 'StdinOnce' should be set.
    request.StdinOnce = true;

    if (WI_IsFlagSet(containerOptions.InitProcessOptions.Flags, WSLCProcessFlagsTty))
    {
        request.Tty = true;
    }

    if (WI_IsFlagSet(containerOptions.InitProcessOptions.Flags, WSLCProcessFlagsStdin))
    {
        request.OpenStdin = true;
    }

    if (containerOptions.InitProcessOptions.CommandLine.Count > 0)
    {
        request.Cmd = StringArrayToVector(containerOptions.InitProcessOptions.CommandLine);
    }

    if (containerOptions.Entrypoint.Count > 0)
    {
        request.Entrypoint = StringArrayToVector(containerOptions.Entrypoint);
    }

    request.Env = StringArrayToVector(containerOptions.InitProcessOptions.Environment);

    if (containerOptions.StopSignal != WSLCSignalNone)
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

    request.HostConfig.Init = WI_IsFlagSet(containerOptions.Flags, WSLCContainerFlagsInit);

    if (containerOptions.VolumesCount > 0)
    {
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, containerOptions.Volumes, "Volumes is null with VolumesCount=%lu", containerOptions.VolumesCount);
    }

    // Build volume list from container options.
    std::vector<WSLCVolumeMount> volumes;
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

        std::filesystem::path hostPath = volume.HostPath;
        THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessagePathNotAbsolute(volume.HostPath), !hostPath.is_absolute());

        std::wstring sourceFilename;

        {
            // Resolve symlinks.
            std::error_code ec;
            hostPath = std::filesystem::canonical(hostPath, ec);
            if (!ec)
            {
                // When the host path is a file, mount the parent directory in the VM
                // and bind only the specific file into the container via Docker.
                if (std::filesystem::is_regular_file(hostPath))
                {
                    sourceFilename = hostPath.filename().wstring();
                    hostPath = hostPath.parent_path();
                }
            }
            else
            {
                if (ec == std::errc::no_such_file_or_directory)
                {
                    // Path doesn't exist, assume directory.
                    hostPath = volume.HostPath;
                }
                else
                {
                    THROW_HR_WITH_USER_ERROR(E_FAIL, Localization::MessageWslcFailedToMountVolume(volume.HostPath, ec.message()));
                }
            }
        }

        volumes.push_back(WSLCVolumeMount{hostPath, parentVMPath, volume.ContainerPath, static_cast<bool>(volume.ReadOnly), sourceFilename});

        auto options = volume.ReadOnly ? "ro" : "rw";
        auto bindSource = sourceFilename.empty() ? parentVMPath : std::format("{}/{}", parentVMPath, sourceFilename);
        auto bind = std::format("{}:{}:{}", bindSource, volume.ContainerPath, options);

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

    ProcessNamedVolumes(containerOptions, sessionVolumes, request);

    // Prepare port mappings from container options.
    std::vector<_WSLCPortMapping> ports;
    for (ULONG i = 0; i < containerOptions.PortsCount; i++)
    {
        auto& port = ports.emplace_back();
        port.HostPort = containerOptions.Ports[i].HostPort;
        port.ContainerPort = containerOptions.Ports[i].ContainerPort;
        port.Family = containerOptions.Ports[i].Family;
        port.Protocol = containerOptions.Ports[i].Protocol;
        strcpy_s(port.BindingAddress, containerOptions.Ports[i].BindingAddress);
    }

    // Append exposed ports from the image, if requested.
    if (WI_IsFlagSet(containerOptions.Flags, WSLCContainerFlagsPublishAll))
    {
        auto imageInfo = DockerClient.InspectImage(containerOptions.Image);

        // Use the resolved image ID so the container is created from the exact same image.
        request.Image = imageInfo.Id;

        if (imageInfo.Config.has_value() && imageInfo.Config->ExposedPorts.has_value())
        {
            for (const auto& [portKey, _] : imageInfo.Config->ExposedPorts.value())
            {
                auto [port, protocol] = ParseExposedPortKey(portKey);

                // Only TCP localhost mappings are currently supported by the relay path.
                if (protocol != IPPROTO_TCP)
                {
                    continue;
                }

                auto& createdPort = ports.emplace_back();
                createdPort.HostPort = WSLC_EPHEMERAL_PORT;
                createdPort.Family = AF_INET;
                createdPort.ContainerPort = port;
                createdPort.Protocol = protocol;
                strcpy_s(createdPort.BindingAddress, "127.0.0.1");
            }
        }
    }

    // Process port mappings from container options.
    auto [mappedPorts, networkMode] = ProcessPortMappings(ports, containerOptions.ContainerNetwork.ContainerNetworkType, virtualMachine);

    request.HostConfig.NetworkMode = networkMode;

    for (const auto& e : mappedPorts)
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

    auto labels = ParseKeyValuePairs(containerOptions.Labels, containerOptions.LabelsCount, WSLCContainerMetadataLabel);

    // Build WSLC metadata to store in a label for recovery on Open().
    WSLCContainerMetadataV1 metadata;
    metadata.Flags = containerOptions.Flags;
    metadata.InitProcessFlags = containerOptions.InitProcessOptions.Flags;
    metadata.Volumes = volumes;

    for (const auto& e : mappedPorts)
    {
        metadata.Ports.emplace_back(e.Serialize());
    }

    request.Labels[WSLCContainerMetadataLabel] = SerializeContainerMetadata(metadata);
    request.Labels.insert(labels.begin(), labels.end());

    // Send the request to docker.
    auto result =
        DockerClient.CreateContainer(request, containerOptions.Name != nullptr ? containerOptions.Name : std::optional<std::string>{});

    // Clean up the Docker container if anything below fails.
    // N.B. The container ID is captured by value since it is moved into the WSLCContainerImpl constructor below.
    auto deleteOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&DockerClient, containerId = result.Id]() {
        DockerClient.DeleteContainer(containerId, true, true);
    });

    // Inspect the container to fetch its generated name (if needed) and Docker's authoritative Created timestamp.
    auto inspectData = DockerClient.InspectContainer(result.Id);

    auto container = std::make_unique<WSLCContainerImpl>(
        wslcSession,
        virtualMachine,
        std::move(result.Id),
        CleanContainerName(inspectData.Name),
        std::string(containerOptions.Image),
        containerOptions.ContainerNetwork.ContainerNetworkType,
        std::move(volumes),
        std::move(mappedPorts),
        std::move(labels),
        std::move(OnDeleted),
        EventTracker,
        DockerClient,
        IoRelay,
        WslcContainerStateCreated,
        ParseDockerTimestamp(inspectData.Created),
        containerOptions.InitProcessOptions.Flags,
        containerOptions.Flags);

    deleteOnFailure.release();
    return container;
}

std::unique_ptr<WSLCContainerImpl> WSLCContainerImpl::Open(
    const common::docker_schema::ContainerInfo& dockerContainer,
    WSLCSession& wslcSession,
    WSLCVirtualMachine& virtualMachine,
    const std::unordered_map<std::string, std::unique_ptr<IWSLCVolume>>& sessionVolumes,
    const std::unordered_set<std::string>& anonymousVolumes,
    std::function<void(const WSLCContainerImpl*)>&& OnDeleted,
    ContainerEventTracker& EventTracker,
    DockerHTTPClient& DockerClient,
    IORelay& ioRelay)
{
    // Extract container name from Docker's names list.
    std::string name = ExtractContainerName(dockerContainer.Names, dockerContainer.Id);

    ValidateNamedVolumes(dockerContainer.Mounts, sessionVolumes, anonymousVolumes);

    auto labels(dockerContainer.Labels);
    auto metadataIt = labels.find(WSLCContainerMetadataLabel);

    THROW_HR_IF_MSG(
        E_INVALIDARG,
        metadataIt == labels.end(),
        "Cannot open WSLC container %hs: missing WSLC metadata label",
        dockerContainer.Id.c_str());

    WI_ASSERT(dockerContainer.State != ContainerState::Running);

    auto metadata = ParseContainerMetadata(metadataIt->second.c_str());
    labels.erase(metadataIt);

    auto networkingMode = DockerNetworkModeToWSLCNetworkType(dockerContainer.HostConfig.NetworkMode);
    // Re-register recovered VM ports in the allocation pool to prevent conflicts.
    std::vector<ContainerPortMapping> ports;
    for (const auto& e : metadata.Ports)
    {
        auto& inserted = ports.emplace_back(ContainerPortMapping{VMPortMapping::FromContainerMetaData(e), e.ContainerPort});

        if (networkingMode == WSLCContainerNetworkTypeBridged)
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

    auto container = std::make_unique<WSLCContainerImpl>(
        wslcSession,
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
        DockerStateToWSLCState(dockerContainer.State),
        static_cast<std::uint64_t>(dockerContainer.Created),
        metadata.InitProcessFlags,
        metadata.Flags);

    // Restore the state change timestamp from Docker inspect data.
    try
    {
        auto inspectData = DockerClient.InspectContainer(dockerContainer.Id);
        auto state = DockerStateToWSLCState(dockerContainer.State);
        const auto& timestamp = (state == WslcContainerStateRunning) ? inspectData.State.StartedAt : inspectData.State.FinishedAt;

        if (!timestamp.empty())
        {
            container->m_stateChangedAt = ParseDockerTimestamp(timestamp);
        }
    }
    CATCH_LOG();

    return container;
}

const std::string& WSLCContainerImpl::ID() const noexcept
{
    return m_id;
}

void WSLCContainerImpl::Inspect(LPSTR* Output) const
{
    auto lock = m_lock.lock_shared();

    try
    {
        // Get Docker inspect data
        auto dockerInspect = m_dockerClient.InspectContainer(m_id);

        // Convert to WSLC schema
        auto wslcInspect = BuildInspectContainer(dockerInspect);

        // Serialize WSLC schema to JSON
        std::string wslcJson = wsl::shared::ToJson(wslcInspect);
        *Output = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(wslcJson.c_str()).release();
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to inspect container '%hs'", m_id.c_str());
}

void WSLCContainerImpl::Logs(WSLCLogsFlags Flags, WSLCHandle* Stdout, WSLCHandle* Stderr, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail) const
{
    auto lock = m_lock.lock_shared();

    wil::unique_socket socket;
    try
    {
        socket = m_dockerClient.ContainerLogs(m_id, Flags, Since, Until, Tail);
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to get logs from '%hs'", m_id.c_str());

    if (WI_IsFlagSet(m_initProcessFlags, WSLCProcessFlagsTty))
    {
        // For tty processes, simply relay the HTTP chunks.
        auto [ttyRead, ttyWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);

        auto handle = std::make_unique<RelayHandle<HTTPChunkBasedReadHandle>>(std::move(socket), std::move(ttyWrite));
        m_ioRelay.AddHandle(std::move(handle));

        *Stdout = common::wslutil::ToCOMOutputHandle(ttyRead.get(), GENERIC_READ | SYNCHRONIZE, WSLCHandleTypePipe);
    }
    else
    {
        // For non-tty process, stdout & stderr are multiplexed.
        auto [stdoutRead, stdoutWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);
        auto [stderrRead, stderrWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);

        auto handle = std::make_unique<DockerIORelayHandle>(
            std::move(socket), std::move(stdoutWrite), std::move(stderrWrite), DockerIORelayHandle::Format::HttpChunked);

        m_ioRelay.AddHandle(std::move(handle));

        *Stdout = common::wslutil::ToCOMOutputHandle(stdoutRead.get(), GENERIC_READ | SYNCHRONIZE, WSLCHandleTypePipe);
        *Stderr = common::wslutil::ToCOMOutputHandle(stderrRead.get(), GENERIC_READ | SYNCHRONIZE, WSLCHandleTypePipe);
    }
}

std::unique_ptr<RelayedProcessIO> WSLCContainerImpl::CreateRelayedProcessIO(wil::unique_handle&& stream, WSLCProcessFlags flags)
{
    // Create one pipe for each STD handle.
    std::vector<std::unique_ptr<OverlappedIOHandle>> ioHandles;
    std::map<ULONG, TypedHandle> fds;

    // This is required for docker to know when stdin is closed.
    auto closeStdin = [socket = stream.get(), this]() {
        LOG_LAST_ERROR_IF(shutdown(reinterpret_cast<SOCKET>(socket), SD_SEND) == SOCKET_ERROR);
    };

    if (WI_IsFlagSet(flags, WSLCProcessFlagsStdin))
    {
        auto [stdinRead, stdinWrite] = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);
        ioHandles.emplace_back(
            std::make_unique<RelayHandle<ReadHandle>>(HandleWrapper{std::move(stdinRead), std::move(closeStdin)}, stream.get()));

        fds.emplace(WSLCFDStdin, TypedHandle{wil::unique_handle{stdinWrite.release()}, WSLCHandleTypePipe});
    }
    else
    {
        // If stdin is not attached, close it now to make sure no one tries to write to it.
        closeStdin();
    }

    auto [stdoutRead, stdoutWrite] = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);
    auto [stderrRead, stderrWrite] = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);

    fds.emplace(WSLCFDStdout, TypedHandle{wil::unique_handle{stdoutRead.release()}, WSLCHandleTypePipe});
    fds.emplace(WSLCFDStderr, TypedHandle{wil::unique_handle{stderrRead.release()}, WSLCHandleTypePipe});

    ioHandles.emplace_back(std::make_unique<DockerIORelayHandle>(
        std::move(stream), std::move(stdoutWrite), std::move(stderrWrite), common::relay::DockerIORelayHandle::Format::Raw));

    m_ioRelay.AddHandles(std::move(ioHandles));

    return std::make_unique<RelayedProcessIO>(std::move(fds));
}

void WSLCContainerImpl::MapPorts()
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

                THROW_HR_WITH_USER_ERROR_IF(
                    HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
                    wsl::shared::Localization::MessageWslcPortInUse(FormatPortEndpoint(e), m_id),
                    !allocatedPort);

                e.VmMapping.AssignVmPort(allocatedPort);

                allocatedPorts.emplace(e.ContainerPort, allocatedPort);
            }
        }

        try
        {
            m_virtualMachine.MapPort(e.VmMapping);
        }
        catch (...)
        {
            auto result = wil::ResultFromCaughtException();
            if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) || result == HRESULT_FROM_WIN32(WSAEADDRINUSE))
            {
                THROW_HR_WITH_USER_ERROR(
                    HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), wsl::shared::Localization::MessageWslcPortInUse(FormatPortEndpoint(e), m_id));
            }
            throw;
        }
    }
}

void WSLCContainerImpl::UnmapPorts()
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
            if (m_networkingMode == WSLCContainerNetworkTypeHost)
            {
                e.VmMapping.VmPort.reset();
            }
        }
        CATCH_LOG();
    }
}

__requires_exclusive_lock_held(m_lock) void WSLCContainerImpl::ReleaseProcesses()
{
    std::lock_guard processesLock{m_processesLock};

    // Notify all processes that the container has exited.
    // The exec callback isn't always sent to execed processes, so do this to avoid 'stuck' processes.
    for (auto& process : m_processes)
    {
        process->OnContainerReleased();
    }

    m_processes.clear();
}

__requires_exclusive_lock_held(m_lock) void WSLCContainerImpl::ReleaseRuntimeResources()
{
    WSL_LOG("ReleaseRuntimeResources", TraceLoggingValue(m_id.c_str(), "ID"));

    // Release runtime resources (port relays, volume mounts) that were set up at Start().
    UnmapPorts();
    UnmountVolumes(m_mountedVolumes, m_virtualMachine);
}

__requires_exclusive_lock_held(m_lock) void WSLCContainerImpl::ReleaseResources()
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

__requires_exclusive_lock_held(m_lock) void WSLCContainerImpl::DisconnectComWrapper()
{
    if (m_comWrapper)
    {
        // Cache read-only properties in the COM wrapper before disconnecting,
        // so callers can still query state/process after the impl is gone.
        {
            std::lock_guard processesLock{m_processesLock};
            m_comWrapper->CacheState(m_id, m_name, m_state, m_initProcess);
        }

        m_comWrapper->Disconnect();
        m_comWrapper.Reset();
    }
}

__requires_lock_held(m_lock) void WSLCContainerImpl::Transition(WSLCContainerState State, std::optional<std::uint64_t> stateChangedAt) noexcept
{
    // N.B. A deleted container cannot transition back to any other state.
    WI_ASSERT(m_state != WslcContainerStateDeleted);

    WSL_LOG(
        "ContainerStateChange",
        TraceLoggingValue(static_cast<int>(m_state), "PreviousState"),
        TraceLoggingValue(static_cast<int>(State), "NewState"),
        TraceLoggingValue(m_id.c_str(), "ID"));

    m_state = State;
    m_stateChangedAt = stateChangedAt.value_or(static_cast<std::uint64_t>(std::time(nullptr)));
}

WSLCContainer::WSLCContainer(WSLCContainerImpl* impl, std::function<void(const WSLCContainerImpl*)>&& OnDeleted) :
    COMImplClass<WSLCContainerImpl>(impl), m_onDeleted(std::move(OnDeleted))
{
}

HRESULT WSLCContainer::Attach(LPCSTR DetachKeys, WSLCHandle* Stdin, WSLCHandle* Stdout, WSLCHandle* Stderr)
{
    COMServiceExecutionContext context;

    *Stdin = {};
    *Stdout = {};
    *Stderr = {};

    return CallImpl(&WSLCContainerImpl::Attach, DetachKeys, Stdin, Stdout, Stderr);
}

HRESULT WSLCContainer::GetState(WSLCContainerState* Result)
{
    COMServiceExecutionContext context;
    RETURN_HR_IF_NULL(E_POINTER, Result);

    *Result = WslcContainerStateInvalid;
    HRESULT hr = CallImpl(&WSLCContainerImpl::GetState, Result);
    if (SUCCEEDED(hr))
    {
        return S_OK;
    }

    // DisconnectComWrapper() populates the cache before setting m_impl to null,
    // so if CallImpl failed with RPC_E_DISCONNECTED, the cache must be populated.
    if (hr == RPC_E_DISCONNECTED)
    {
        auto cacheLock = m_cacheLock.lock_shared();
        if (WI_VERIFY(m_cachedState.has_value()))
        {
            *Result = m_cachedState.value();
            return S_OK;
        }
    }

    return hr;
}

HRESULT WSLCContainer::GetInitProcess(IWSLCProcess** Process)
{
    COMServiceExecutionContext context;

    *Process = nullptr;

    HRESULT hr = CallImpl(&WSLCContainerImpl::GetInitProcess, Process);
    if (SUCCEEDED(hr))
    {
        return S_OK;
    }

    // DisconnectComWrapper() populates the cache before setting m_impl to null,
    // so if CallImpl failed with RPC_E_DISCONNECTED, the cache must be populated.
    if (hr == RPC_E_DISCONNECTED)
    {
        auto cacheLock = m_cacheLock.lock_shared();
        if (m_cachedInitProcess)
        {
            return m_cachedInitProcess.CopyTo(__uuidof(IWSLCProcess), (void**)Process);
        }
    }

    return hr;
}

HRESULT WSLCContainer::Exec(const WSLCProcessOptions* Options, LPCSTR DetachKeys, IWSLCProcess** Process)
{
    COMServiceExecutionContext context;

    *Process = nullptr;
    return CallImpl(&WSLCContainerImpl::Exec, Options, DetachKeys, Process);
}

HRESULT WSLCContainer::Stop(_In_ WSLCSignal Signal, _In_ LONG TimeoutSeconds)
{
    COMServiceExecutionContext context;

    return CallImpl(&WSLCContainerImpl::Stop, Signal, TimeoutSeconds, false);
}

HRESULT WSLCContainer::Kill(_In_ WSLCSignal Signal)
{
    COMServiceExecutionContext context;

    return CallImpl(&WSLCContainerImpl::Stop, Signal, {}, true);
}

HRESULT WSLCContainer::Start(WSLCContainerStartFlags Flags, LPCSTR DetachKeys)
try
{
    COMServiceExecutionContext context;

    THROW_HR_IF_MSG(E_INVALIDARG, WI_IsAnyFlagSet(Flags, ~WSLCContainerStartFlagsValid), "Invalid flags: 0x%x", Flags);

    return CallImpl(&WSLCContainerImpl::Start, Flags, DetachKeys);
}
CATCH_RETURN();

HRESULT WSLCContainer::Inspect(LPSTR* Output)
{
    COMServiceExecutionContext context;

    *Output = nullptr;

    return CallImpl(&WSLCContainerImpl::Inspect, Output);
}

HRESULT WSLCContainer::Delete(WSLCDeleteFlags Flags)
try
{
    COMServiceExecutionContext context;

    THROW_HR_IF_MSG(E_INVALIDARG, WI_IsAnyFlagSet(Flags, ~WSLCDeleteFlagsValid), "Invalid flags: 0x%x", Flags);

    // Special case for Delete(): If deletion is successful, notify the WSLCSession that the container has been deleted.
    auto [lock, impl] = LockImpl();

    impl->Delete(Flags);
    m_onDeleted(impl);

    return S_OK;
}
CATCH_RETURN();

void WSLCContainer::CacheState(const std::string& id, const std::string& name, WSLCContainerState state, const Microsoft::WRL::ComPtr<IWSLCProcess>& initProcess) noexcept
try
{
    auto cacheLock = m_cacheLock.lock_exclusive();

    // CacheState must only be called once, during DisconnectComWrapper().
    WI_ASSERT(!m_cachedState.has_value());

    m_cachedId = id;
    m_cachedName = name;
    m_cachedState = state;
    m_cachedInitProcess = initProcess;
}
CATCH_LOG();

HRESULT WSLCContainer::Export(WSLCHandle TarHandle)
{
    COMServiceExecutionContext context;

    return CallImpl(&WSLCContainerImpl::Export, TarHandle);
}

HRESULT WSLCContainer::Logs(WSLCLogsFlags Flags, WSLCHandle* Stdout, WSLCHandle* Stderr, ULONGLONG Since, ULONGLONG Until, ULONGLONG Tail)
try
{
    COMServiceExecutionContext context;
    RETURN_HR_IF(E_POINTER, Stdout == nullptr || Stderr == nullptr);

    THROW_HR_IF_MSG(E_INVALIDARG, WI_IsAnyFlagSet(Flags, ~WSLCLogsFlagsValid), "Invalid flags: 0x%x", Flags);

    *Stdout = {};
    *Stderr = {};

    return CallImpl(&WSLCContainerImpl::Logs, Flags, Stdout, Stderr, Since, Until, Tail);
}
CATCH_RETURN();

HRESULT WSLCContainer::GetId(WSLCContainerId Id)
try
{
    COMServiceExecutionContext context;

    const auto hr = wil::ResultFromException([&] {
        auto [lock, impl] = LockImpl();
        WI_VERIFY(strcpy_s(Id, std::size<char>(WSLCContainerId{}), impl->ID().c_str()) == 0);
    });

    RETURN_HR_IF(hr, hr != RPC_E_DISCONNECTED);

    // DisconnectComWrapper() populates the cache before setting m_impl to null,
    // so if LockImpl failed with RPC_E_DISCONNECTED, the cache must be populated.
    auto cacheLock = m_cacheLock.lock_shared();
    if (WI_VERIFY(m_cachedId.has_value()))
    {
        WI_VERIFY(strcpy_s(Id, std::size<char>(WSLCContainerId{}), m_cachedId->c_str()) == 0);
        return S_OK;
    }

    return hr;
}
CATCH_RETURN();

HRESULT WSLCContainer::GetName(LPSTR* Name)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF_NULL(E_POINTER, Name);
    *Name = nullptr;

    const auto hr = wil::ResultFromException([&] {
        auto [lock, impl] = LockImpl();
        *Name = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(impl->Name().c_str()).release();
    });

    RETURN_HR_IF(hr, hr != RPC_E_DISCONNECTED);

    // DisconnectComWrapper() populates the cache before setting m_impl to null,
    // so if LockImpl failed with RPC_E_DISCONNECTED, the cache must be populated.
    auto cacheLock = m_cacheLock.lock_shared();
    if (WI_VERIFY(m_cachedName.has_value()))
    {
        *Name = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(m_cachedName->c_str()).release();
        return S_OK;
    }

    return hr;
}
CATCH_RETURN();

void WSLCContainerImpl::GetLabels(WSLCLabelInformation** Labels, ULONG* Count) const
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
    auto labelsArray = wil::make_unique_cotaskmem<WSLCLabelInformation[]>(localLabels.size());
    for (size_t i = 0; i < localLabels.size(); ++i)
    {
        labelsArray[i].Key = localLabels[i].first.release();
        labelsArray[i].Value = localLabels[i].second.release();
    }

    *Count = static_cast<ULONG>(localLabels.size());
    *Labels = labelsArray.release();
}

HRESULT WSLCContainer::GetLabels(WSLCLabelInformation** Labels, ULONG* Count)
try
{
    COMServiceExecutionContext context;

    RETURN_HR_IF(E_POINTER, Labels == nullptr || Count == nullptr);

    *Count = 0;
    *Labels = nullptr;
    return CallImpl(&WSLCContainerImpl::GetLabels, Labels, Count);
}
CATCH_RETURN();

HRESULT WSLCContainer::InterfaceSupportsErrorInfo(REFIID riid)
{
    return riid == __uuidof(IWSLCContainer) ? S_OK : S_FALSE;
}
