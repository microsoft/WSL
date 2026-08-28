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
#include "WSLCExecutionContext.h"
#include "WSLCProcess.h"
#include "WSLCProcessIO.h"
#include "WSLCVolumes.h"
#include "APICompat.h"
#include "MountSpecParsing.h"
#include <unordered_set>

namespace apicompat = wsl::windows::common::apicompat;

using wsl::windows::common::COMServiceExecutionContext;
using wsl::windows::common::docker_schema::ErrorResponse;
using wsl::windows::common::io::DockerIORelayHandle;
using wsl::windows::common::io::HandleWrapper;
using wsl::windows::common::io::HTTPChunkBasedReadHandle;
using wsl::windows::common::io::OverlappedIOHandle;
using wsl::windows::common::io::ReadHandle;
using wsl::windows::common::io::RelayHandle;
using wsl::windows::service::wslc::ContainerPortMapping;
using wsl::windows::service::wslc::DockerEventTracker;
using wsl::windows::service::wslc::DockerHTTPClient;
using wsl::windows::service::wslc::DockerHTTPException;
using wsl::windows::service::wslc::IORelay;
using wsl::windows::service::wslc::IWSLCVolume;
using wsl::windows::service::wslc::NetworkEntry;
using wsl::windows::service::wslc::RelayedProcessIO;
using wsl::windows::service::wslc::TypedHandle;
using wsl::windows::service::wslc::unique_com_disconnect;
using wsl::windows::service::wslc::VMPortMapping;
using wsl::windows::service::wslc::WSLCContainer;
using wsl::windows::service::wslc::WSLCContainerImpl;
using wsl::windows::service::wslc::WSLCContainerMetadata;
using wsl::windows::service::wslc::WSLCContainerMetadataLabel;
using wsl::windows::service::wslc::WSLCContainerMetadataV1;
using wsl::windows::service::wslc::WSLCExecutionContext;
using wsl::windows::service::wslc::WSLCSession;
using wsl::windows::service::wslc::WSLCVirtualMachine;
using wsl::windows::service::wslc::WSLCVolumeMount;
using wsl::windows::service::wslc::WSLCVolumes;

using namespace wsl::windows::common::io;
using namespace wsl::windows::common::docker_schema;
using namespace wsl::windows::common::wslutil;
using namespace std::chrono_literals;
using wsl::shared::Localization;

namespace wslc_schema = wsl::windows::common::wslc_schema;

using DockerInspectContainer = wsl::windows::common::docker_schema::InspectContainer;
using WslcInspectContainer = wsl::windows::common::wslc_schema::InspectContainer;

namespace {

void ValidateStopTimeout(LONG TimeoutSeconds, bool allowDefault)
{
    THROW_HR_WITH_USER_ERROR_IF(
        E_INVALIDARG,
        Localization::MessageWslcInvalidStopTimeout(TimeoutSeconds),
        TimeoutSeconds < 0 && TimeoutSeconds != WSLC_STOP_TIMEOUT_NONE && (!allowDefault || TimeoutSeconds != WSLC_STOP_TIMEOUT_DEFAULT));
}

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
    wil::unique_socket sock(::socket(family, SOCK_STREAM, IPPROTO_TCP));
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

constexpr std::string_view c_containerNetworkPrefix = "container:";

bool NetworkModeAllocatesVmPorts(std::string_view mode) noexcept
{
    return mode != "host" && mode != "none" && !mode.starts_with(c_containerNetworkPrefix);
}

bool NetworkSupportsAliases(std::string_view mode) noexcept
{
    return mode != "bridge" && NetworkModeAllocatesVmPorts(mode);
}

// Reject `<prefix>:<value>` strings whose prefix isn't `container:`. Docker treats colon-prefixed
// modes (`service:`, `ns:`, ...) as special, but WSLC only supports `container:`. Surface the
// rejection here so both Create() and Open() recovery paths share the same gate.
void RejectUnsupportedNetworkModes(std::string_view mode)
{
    if (mode.starts_with(c_containerNetworkPrefix))
    {
        return;
    }

    const auto colon = mode.find(':');
    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcInvalidNetworkMode(std::string{mode}), colon != std::string_view::npos);
}

std::string ResolveNetworkMode(LPCSTR networkMode, bool hasRequestedPorts, const std::unordered_map<std::string, NetworkEntry>& sessionNetworks, DockerHTTPClient& dockerClient)
{
    const std::string_view mode = (networkMode == nullptr || *networkMode == '\0') ? std::string_view{"bridge"} : networkMode;

    // Reject `service:foo` and similar unsupported colon-prefixed modes before any further processing.
    RejectUnsupportedNetworkModes(mode);

    // N.B. Docker validates incompatible combinations (e.g. host/none/container: with additional networks)
    // and returns clear error messages, so we don't duplicate that validation here.

    if (mode == "host")
    {
        return "host";
    }

    if (mode == "none")
    {
        THROW_HR_IF_MSG(E_INVALIDARG, hasRequestedPorts, "Port mappings are not supported without networking");
        return "none";
    }

    if (mode.starts_with(c_containerNetworkPrefix))
    {
        const std::string target{mode.substr(c_containerNetworkPrefix.size())};
        THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcContainerModeRequiresTarget(), target.empty());
        THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcContainerModeNoPorts(), hasRequestedPorts);

        try
        {
            return std::format("container:{}", dockerClient.InspectContainer(target).Id);
        }
        catch (const DockerHTTPException& e)
        {
            THROW_HR_WITH_USER_ERROR_IF(
                WSLC_E_CONTAINER_NOT_FOUND, Localization::MessageWslcContainerModeTargetNotFound(target), e.StatusCode() == 404);
            throw;
        }
    }

    // User-defined network: bridge is the built-in one and bypasses the session lookup.
    if (mode != "bridge")
    {
        THROW_HR_WITH_USER_ERROR_IF(
            WSLC_E_NETWORK_NOT_FOUND, Localization::MessageWslcNetworkNotFound(std::string{mode}), !sessionNetworks.contains(std::string{mode}));
    }
    return std::string{mode};
}

// Unknown Settings keys are rejected rather than silently dropped, so callers get a clear error.
EndpointConfig ResolveEndpointConfig(const KeyValuePair* settings, ULONG count, std::string_view networkName)
{
    EndpointConfig config{};
    if (count == 0)
    {
        return config;
    }

    THROW_HR_IF_MSG(E_INVALIDARG, settings == nullptr, "Settings is null with SettingsCount=%lu", count);

    auto parsed = ParseKeyMultiValuePairs(settings, count);

    static constexpr std::array knownKeys{"Aliases", "IPAddress", "Links", "LinkLocalIPs", "DriverOpts"};
    for (const auto& [key, _] : parsed)
    {
        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG,
            Localization::MessageWslcEndpointSettingUnknown(key, std::string{networkName}),
            std::find(knownKeys.begin(), knownKeys.end(), key) == knownKeys.end());
    }

    auto isBlank = [](const std::string& value) {
        return value.empty() || std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch); });
    };

    if (auto it = parsed.find("Aliases"); it != parsed.end())
    {
        for (const auto& alias : it->second)
        {
            THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcAliasEmpty(), isBlank(alias));
        }

        config.Aliases = std::move(it->second);
    }

    if (auto it = parsed.find("IPAddress"); it != parsed.end())
    {
        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG, Localization::MessageWslcIpAddressSingleValue(std::string{networkName}), it->second.size() != 1);

        const auto& address = it->second.front();
        in_addr parsedAddress{};
        ParseIpv4Address(address.c_str(), parsedAddress);

        EndpointIPAMConfig ipam{};
        ipam.IPv4Address = address;
        config.IPAMConfig = std::move(ipam);
    }

    if (auto it = parsed.find("Links"); it != parsed.end())
    {
        for (const auto& link : it->second)
        {
            THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcLinkEmpty(), isBlank(link));
        }
        config.Links = std::move(it->second);
    }

    if (auto it = parsed.find("LinkLocalIPs"); it != parsed.end())
    {
        for (const auto& address : it->second)
        {
            in_addr parsedAddress{};
            ParseIpv4Address(address.c_str(), parsedAddress);
        }
        if (!config.IPAMConfig.has_value())
        {
            config.IPAMConfig = EndpointIPAMConfig{};
        }
        config.IPAMConfig->LinkLocalIPs = std::move(it->second);
    }

    if (auto it = parsed.find("DriverOpts"); it != parsed.end())
    {
        std::map<std::string, std::string> driverOpts;
        for (const auto& entry : it->second)
        {
            const auto separator = entry.find('=');
            THROW_HR_WITH_USER_ERROR_IF(
                E_INVALIDARG, Localization::MessageWslcDriverOptInvalid(entry), separator == std::string::npos || separator == 0);

            auto key = entry.substr(0, separator);
            auto value = entry.substr(separator + 1);
            THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcDriverOptInvalid(entry), isBlank(key));
            THROW_HR_WITH_USER_ERROR_IF(
                E_INVALIDARG, Localization::MessageWslcDriverOptDuplicate(key), !driverOpts.try_emplace(key, std::move(value)).second);
        }
        config.DriverOpts = std::move(driverOpts);
    }

    return config;
}

std::map<std::string, EndpointConfig> ResolveEndpoints(
    const WSLCNetworkConnection* connections, ULONG count, std::string_view resolvedMode, const std::unordered_map<std::string, NetworkEntry>& sessionNetworks)
{
    std::map<std::string, EndpointConfig> resolved;
    if (count == 0)
    {
        return resolved;
    }

    THROW_HR_IF_MSG(E_INVALIDARG, connections == nullptr, "Networks is null with NetworksCount=%lu", count);

    for (ULONG i = 0; i < count; i++)
    {
        const char* raw = connections[i].NetworkName;
        THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcNetworkNameRequired(), !raw || !*raw);

        std::string name{raw};
        THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcDuplicateNetwork(name), name == resolvedMode);

        auto [it, inserted] = resolved.try_emplace(name);
        THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcDuplicateNetwork(name), !inserted);

        auto config = ResolveEndpointConfig(connections[i].Settings, connections[i].SettingsCount, name);
        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG, Localization::MessageWslcAliasRequiresUserDefinedNetwork(), config.Aliases.has_value() && !NetworkSupportsAliases(name));

        if (name != "bridge")
        {
            THROW_HR_WITH_USER_ERROR_IF(
                WSLC_E_NETWORK_NOT_FOUND, Localization::MessageWslcNetworkNotFound(name), !sessionNetworks.contains(name));
        }

        it->second = std::move(config);
    }
    return resolved;
}

// Builds the port-mapping list from caller-supplied requests.
std::vector<ContainerPortMapping> BuildPortMappings(std::vector<_WSLCPortMapping>& requestedPorts, std::string_view primary, WSLCVirtualMachine& vm)
{
    std::vector<ContainerPortMapping> ports;
    ports.reserve(requestedPorts.size());

    const bool allocateVmPorts = NetworkModeAllocatesVmPorts(primary);
    for (auto& e : requestedPorts)
    {
        // Pre-allocate a concrete host port whenever the wslrelay relay path will be used: that path
        // maps the host port verbatim and has no ephemeral writeback (unlike the virtioNet path), so
        // an unresolved WSLC_EPHEMERAL_PORT (0) would otherwise be mapped as port 0 and fail.
        if (e.HostPort == WSLC_EPHEMERAL_PORT && vm.UseWslRelayPortForwarding())
        {
            e.HostPort = AllocateEphemeralPort(e.Family, e.BindingAddress);
        }

        auto& entry = ports.emplace_back(VMPortMapping::FromWSLCPortMapping(e), e.ContainerPort);
        if (allocateVmPorts)
        {
            entry.VmMapping.AssignVmPort(vm.AllocatePort(e.Family, e.Protocol));
        }
    }
    return ports;
}

void UnmountVolumes(std::vector<WSLCVolumeMount>& volumes, WSLCVirtualMachine& parentVM)
{
    for (auto& volume : volumes)
    {
        if (volume.Mounted)
        {
            auto result = parentVM.UnmountWindowsFolder(volume.ParentVMPath.c_str());
            if (SUCCEEDED(result))
            {
                volume.Mounted = false;
            }
            else
            {
                LOG_HR(result);
                EMIT_USER_WARNING(wsl::shared::Localization::MessageWslcVolumeUnmountFailed(
                    volume.HostPath, wsl::windows::common::wslutil::GetErrorString(result)));
            }
        }
    }
}

auto MountVolumes(std::vector<WSLCVolumeMount>& volumes, WSLCVirtualMachine& parentVM)
{
    auto errorCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&volumes, &parentVM]() { UnmountVolumes(volumes, parentVM); });

    for (auto& volume : volumes)
    {
        std::error_code error;
        const auto sourceExists = std::filesystem::exists(volume.HostPath, error);
        if (error)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcBindSourcePathError(volume.HostPath, error.message()));
        }

        if (!sourceExists)
        {
            if (!volume.CreateSourceIfMissing)
            {
                THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageWslcBindSourcePathNotFound(volume.HostPath));
            }

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

std::map<std::string, std::string> StripInternalLabels(std::map<std::string, std::string> labels)
{
    labels.erase(WSLCContainerMetadataLabel);
    return labels;
}

std::map<std::string, std::string> StripInternalLabels(std::optional<std::map<std::string, std::string>>&& labels)
{
    return StripInternalLabels(std::move(labels).value_or(std::map<std::string, std::string>{}));
}

// Validate every mount representation as one collection before preparing VM shares or calling Docker.
// Docker handles duplicate destinations differently across Binds, Mounts, and Tmpfs and can create named volumes while processing the request.
// This service-boundary check gives every caller consistent duplicate semantics and keeps invalid requests side-effect free.
std::vector<wsl::windows::common::mount::Spec> ConvertAndValidateMounts(const WSLCContainerOptions& containerOptions)
{
    namespace mount = wsl::windows::common::mount;

    THROW_HR_IF(E_INVALIDARG, containerOptions.MountsCount > 0 && containerOptions.Mounts == nullptr);

    std::vector<mount::Spec> mounts;
    mounts.reserve(containerOptions.MountsCount);
    for (ULONG i = 0; i < containerOptions.MountsCount; ++i)
    {
        const auto& value = containerOptions.Mounts[i];
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, value.Target, "Mount at index %lu has null Target", i);
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            WI_IsAnyFlagSet(value.Flags, ~WSLCMountSpecFlagsValid),
            "Mount at index %lu has invalid flags: 0x%x",
            i,
            value.Flags);

        switch (value.Type)
        {
        case WSLCMountTypeBind:
        case WSLCMountTypeVolume:
        case WSLCMountTypeTmpfs:
            break;

        default:
            THROW_HR_MSG(E_INVALIDARG, "Mount at index %lu has invalid type: %d", i, value.Type);
        }

        const auto type = value.Type;
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            type != WSLCMountTypeBind && WI_IsFlagSet(value.Flags, WSLCMountSpecFlagsCreateSourceIfMissing),
            "Mount at index %lu specifies create-source-if-missing for a non-bind mount",
            i);
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            type != WSLCMountTypeTmpfs && value.TmpfsOptions != nullptr,
            "Mount at index %lu specifies tmpfs options for a non-tmpfs mount",
            i);
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            value.TmpfsOptions != nullptr && WI_IsAnyFlagSet(value.Flags, WSLCMountSpecFlagsTmpfsSize | WSLCMountSpecFlagsTmpfsMode),
            "Mount at index %lu combines legacy and structured tmpfs options",
            i);

        mounts.push_back({
            .MountType = type,
            .Source = value.Source != nullptr ? value.Source : L"",
            .Target = value.Target,
            .ReadOnly = static_cast<bool>(value.ReadOnly),
            .BindSource = WI_IsFlagSet(value.Flags, WSLCMountSpecFlagsCreateSourceIfMissing) ? mount::BindSourcePolicy::CreateIfMissing
                                                                                             : mount::BindSourcePolicy::RequireExisting,
            .TmpfsSizeBytes = WI_IsFlagSet(value.Flags, WSLCMountSpecFlagsTmpfsSize) ? std::optional<int64_t>{value.TmpfsSizeBytes} : std::nullopt,
            .TmpfsMode = WI_IsFlagSet(value.Flags, WSLCMountSpecFlagsTmpfsMode) ? std::optional<uint32_t>{value.TmpfsMode} : std::nullopt,
            .TmpfsOptions = value.TmpfsOptions != nullptr ? std::optional<std::string>{value.TmpfsOptions} : std::nullopt,
        });
    }

    try
    {
        mount::ValidateMountCollection(mounts);
        for (const auto& mount : mounts)
        {
            if (mount.MountType == WSLCMountTypeBind)
            {
                if (mount.BindSource == mount::BindSourcePolicy::CreateIfMissing)
                {
                    continue;
                }

                std::error_code error;
                const auto sourceExists = std::filesystem::exists(mount.Source, error);
                if (error)
                {
                    throw mount::MountValidationException(Localization::MessageWslcBindSourcePathError(mount.Source, error.message()));
                }

                if (!sourceExists)
                {
                    throw mount::MountValidationException(Localization::MessageWslcBindSourcePathNotFound(mount.Source));
                }
            }
        }
    }
    catch (const mount::MountException& ex)
    {
        if (ex.Error() == mount::ValidationError::DuplicateDestination)
        {
            THROW_HR_WITH_USER_ERROR(
                E_INVALIDARG, Localization::WSLCCLI_DuplicateMountDestinationError(wsl::shared::string::MultiByteToWide(ex.Destination())));
        }

        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, ex.Reason());
    }

    std::unordered_set<std::string> destinations;
    const auto addDestination = [&](const char* destination) {
        THROW_HR_IF_NULL(E_INVALIDARG, destination);

        const auto normalizedDestination = mount::NormalizeDestination(destination);
        THROW_HR_WITH_USER_ERROR_IF(
            E_INVALIDARG,
            Localization::WSLCCLI_DuplicateMountDestinationError(wsl::shared::string::MultiByteToWide(normalizedDestination)),
            !destinations.emplace(normalizedDestination).second);
    };

    for (const auto& mount : mounts)
    {
        addDestination(mount.Target.c_str());
    }

    THROW_HR_IF(E_INVALIDARG, containerOptions.VolumesCount > 0 && containerOptions.Volumes == nullptr);
    for (ULONG i = 0; i < containerOptions.VolumesCount; ++i)
    {
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, containerOptions.Volumes[i].HostPath, "Volumes[%lu].HostPath is null", i);
        addDestination(containerOptions.Volumes[i].ContainerPath);
    }

    THROW_HR_IF(E_INVALIDARG, containerOptions.NamedVolumesCount > 0 && containerOptions.NamedVolumes == nullptr);
    for (ULONG i = 0; i < containerOptions.NamedVolumesCount; ++i)
    {
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, containerOptions.NamedVolumes[i].Name, "NamedVolume at index %lu has null Name", i);
        addDestination(containerOptions.NamedVolumes[i].ContainerPath);
    }

    THROW_HR_IF(E_INVALIDARG, containerOptions.TmpfsCount > 0 && containerOptions.Tmpfs == nullptr);
    for (ULONG i = 0; i < containerOptions.TmpfsCount; ++i)
    {
        addDestination(containerOptions.Tmpfs[i].Destination);
    }

    return mounts;
}

struct PreparedBindMount
{
    WSLCVolumeMount Volume;
    std::string DockerSource;
};

enum class MissingBindSource
{
    Create,
    Reject,
};

PreparedBindMount PrepareBindMount(const std::wstring& source, const std::string& target, bool readOnly, MissingBindSource missingSource)
{
    std::filesystem::path hostPath = source;
    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessagePathNotAbsolute(source), !hostPath.is_absolute());

    std::wstring sourceFilename;
    {
        std::error_code ec;
        hostPath = wsl::windows::common::filesystem::GetCanonicalPath(hostPath, ec);
        if (ec)
        {
            THROW_HR_WITH_USER_ERROR(HRESULT_FROM_WIN32(ec.value()), Localization::MessageWslcFailedToMountVolume(source, ec.message()));
        }

        if (std::filesystem::is_regular_file(hostPath))
        {
            sourceFilename = hostPath.filename().wstring();
            hostPath = hostPath.parent_path();
        }
    }

    GUID volumeId;
    THROW_IF_FAILED(CoCreateGuid(&volumeId));
    auto parentVMPath = std::format("/mnt/{}", wsl::shared::string::GuidToString<char>(volumeId));
    auto dockerSource = sourceFilename.empty() ? parentVMPath : std::format("{}/{}", parentVMPath, sourceFilename);
    return {
        .Volume =
            {
                .HostPath = std::move(hostPath),
                .ParentVMPath = std::move(parentVMPath),
                .ContainerPath = target,
                .ReadOnly = readOnly,
                .SourceFilename = std::move(sourceFilename),
                .CreateSourceIfMissing = missingSource == MissingBindSource::Create,
            },
        .DockerSource = std::move(dockerSource),
    };
}

void ProcessNamedVolumes(const WSLCContainerOptions& containerOptions, wsl::windows::common::docker_schema::CreateContainer& request)
{
    THROW_HR_IF(E_INVALIDARG, containerOptions.NamedVolumesCount > 0 && containerOptions.NamedVolumes == nullptr);

    for (ULONG i = 0; i < containerOptions.NamedVolumesCount; i++)
    {
        const auto& nv = containerOptions.NamedVolumes[i];
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, nv.Name, "NamedVolume at index %lu has null Name", i);
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, nv.ContainerPath, "NamedVolume at index %lu has null ContainerPath", i);

        wsl::windows::common::docker_schema::Mount mount{};
        mount.Source = std::string(nv.Name);
        mount.Target = std::string(nv.ContainerPath);
        mount.Type = "volume";
        mount.ReadOnly = static_cast<bool>(nv.ReadOnly);

        request.HostConfig.Mounts.emplace_back(mount);
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

unique_com_disconnect::unique_com_disconnect(Microsoft::WRL::ComPtr<WSLCContainer>&& wrapper) noexcept :
    m_wrapper(std::move(wrapper))
{
}

unique_com_disconnect::~unique_com_disconnect() noexcept
{
    if (m_wrapper)
    {
        m_wrapper->Disconnect();
    }
}

wsl::windows::service::wslc::WSLCPortMapping ContainerPortMapping::Serialize() const
{
    return wsl::windows::service::wslc::WSLCPortMapping{
        .HostPort = VmMapping.HostPort(),
        .VmPort = VmMapping.VmPort ? VmMapping.VmPort->Port() : ContainerPort,
        .ContainerPort = ContainerPort,
        .Family = VmMapping.BindAddress.si_family,
        .Protocol = VmMapping.Protocol,
        .BindingAddress = VmMapping.BindingAddressString()};
}

WSLCContainerImpl::WSLCContainerImpl(
    WSLCSession& wslcSession,
    WSLCSessionRuntime& runtime,
    IWSLCPluginNotifier* pluginNotifier,
    std::string&& Id,
    std::string&& Name,
    std::string&& Image,
    std::string NetworkMode,
    std::vector<WSLCVolumeMount>&& volumes,
    std::vector<std::string>&& namedVolumes,
    std::vector<ContainerPortMapping>&& ports,
    std::map<std::string, std::string>&& labels,
    std::function<void(const WSLCContainerImpl*)>&& onDeleted,
    WSLCContainerState InitialState,
    std::int64_t CreatedAt,
    WSLCProcessFlags InitProcessFlags,
    WSLCContainerFlags ContainerFlags) :
    m_wslcSession(wslcSession),
    m_pluginNotifier(pluginNotifier),
    m_runtime(runtime),
    m_name(std::move(Name)),
    m_image(std::move(Image)),
    m_networkMode(std::move(NetworkMode)),
    m_id(std::move(Id)),
    m_mountedVolumes(std::move(volumes)),
    m_namedVolumes(std::move(namedVolumes)),
    m_mappedPorts(std::move(ports)),
    m_labels(std::move(labels)),
    m_comWrapper(wil::MakeOrThrow<WSLCContainer>(wslcSession, std::move(onDeleted))),
    m_containerEvents(runtime.Events().RegisterContainerStateUpdates(
        m_id, std::bind(&WSLCContainerImpl::OnEvent, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3))),
    m_state(InitialState),
    m_createdAt(CreatedAt),
    m_initProcessFlags(InitProcessFlags),
    m_containerFlags(ContainerFlags)
{
    // Acquire the activity hold up front for a container recovered in the running state, so it keeps
    // the VM alive even before any client opens its wrapper. A merely-created (never-started)
    // container does not pin the VM: its metadata survives teardown and the VM restarts on next use.
    if (m_state == WslcContainerStateRunning)
    {
        m_activityHold = ActivityRef(m_wslcSession.Runtime().IdleStateShared());
    }
}

WSLCContainerImpl::~WSLCContainerImpl()
{
    // Destructors are implicitly noexcept, so any escaping exception terminates the session host.
    // Everything below touches VM-scoped state that may already be gone.
    try
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
            if (auto control = process.lock())
            {
                control->OnContainerReleased();
            }
        }

        m_containerEvents.Reset();

        // Release resources under m_lock, but extract the COM wrapper so Disconnect()
        // can be called without holding m_lock. Calling Disconnect() under m_lock can
        // deadlock if an in-flight COM caller is waiting for m_lock.
        unique_com_disconnect wrapper;
        {
            auto lock = m_lock.lock_exclusive();
            wrapper = ReleaseResources();
        }
    }
    CATCH_LOG()
}

void WSLCContainerImpl::Initialize()
{
    // N.B. this must be done here because weak_from_this() is only valid after the constructor returns.
    m_comWrapper->Initialize(weak_from_this());
}

void WSLCContainerImpl::SetExitCode(int ExitCode) noexcept
{
    std::lock_guard processesLock{m_processesLock};
    if (m_initProcessControl != nullptr)
    {
        m_initProcessControl->SetExitCode(ExitCode);
    }
}

void WSLCContainerImpl::SignalInitProcessExit() noexcept
{
    std::lock_guard processesLock{m_processesLock};
    if (m_initProcessControl != nullptr)
    {
        m_initProcessControl->SignalExit();
    }
}

const std::string& WSLCContainerImpl::Image() const noexcept
{
    return m_image;
}

const std::string& WSLCContainerImpl::Name() const noexcept
{
    return m_name;
}

std::vector<wsl::windows::service::wslc::WSLCPortMapping> WSLCContainerImpl::GetPorts() const
{
    auto lock = m_lock.lock_shared();
    if (m_state != WslcContainerStateRunning)
    {
        return {};
    }

    std::vector<wsl::windows::service::wslc::WSLCPortMapping> result;
    result.reserve(m_mappedPorts.size());
    for (const auto& port : m_mappedPorts)
    {
        result.push_back(port.Serialize());
    }
    return result;
}

void WSLCContainerImpl::GetStateChangedAt(LONGLONG* Result)
{
    auto lock = m_lock.lock_shared();
    *Result = m_stateChangedAt;
}

void WSLCContainerImpl::GetCreatedAt(LONGLONG* Result)
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

    wil::shared_socket ioHandle;

    try
    {
        ioHandle = wil::shared_socket{
            m_runtime.Docker().AttachContainer(m_id, DetachKeys == nullptr ? std::nullopt : std::optional<std::string>(DetachKeys))};
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
    auto onInputComplete = [ioHandle]() { LOG_LAST_ERROR_IF(shutdown(ioHandle.get(), SD_SEND) == SOCKET_ERROR); };

    handles.emplace_back(std::make_unique<RelayHandle<ReadHandle>>(
        HandleWrapper{std::move(stdinRead), std::move(onInputComplete)}, HandleWrapper{ioHandle}));

    handles.emplace_back(std::make_unique<DockerIORelayHandle>(
        HandleWrapper{ioHandle}, std::move(stdoutWrite), std::move(stderrWrite), DockerIORelayHandle::Format::Raw));

    m_runtime.Relay()->AddHandles(std::move(handles));

    *Stdin = common::wslutil::ToCOMOutputHandle(reinterpret_cast<HANDLE>(stdinWrite.get()), GENERIC_WRITE | SYNCHRONIZE, WSLCHandleTypePipe);

    *Stdout = common::wslutil::ToCOMOutputHandle(reinterpret_cast<HANDLE>(stdoutRead.get()), GENERIC_READ | SYNCHRONIZE, WSLCHandleTypePipe);

    *Stderr = common::wslutil::ToCOMOutputHandle(reinterpret_cast<HANDLE>(stderrRead.get()), GENERIC_READ | SYNCHRONIZE, WSLCHandleTypePipe);
}

void WSLCContainerImpl::Start(WSLCContainerStartFlags Flags, const WSLCProcessStartOptions* StartOptions)
{
    std::shared_ptr<StateTransition> transition;
    auto lifecycleLock = m_lifecycleLock.lock_shared();
    auto lock = m_lock.lock_exclusive();
    WaitForConflictingTransitionToComplete(lock, lifecycleLock);

    THROW_HR_WITH_USER_ERROR_IF(WSLC_E_CONTAINER_IS_RUNNING, Localization::MessageWslcContainerIsRunning(m_id), m_state == WslcContainerStateRunning);

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        m_state != WslcContainerStateCreated && m_state != WslcContainerStateExited,
        "Cannot start container '%hs', state %i",
        m_id.c_str(),
        m_state);

    std::optional<std::string> detachKeys;

    if (StartOptions != nullptr)
    {
        detachKeys = StartOptions->DetachKeys != nullptr ? std::optional<std::string>(StartOptions->DetachKeys) : std::nullopt;

        THROW_HR_IF_MSG(
            E_INVALIDARG,
            WI_IsFlagSet(m_initProcessFlags, WSLCProcessFlagsTty) && (StartOptions->TtyColumns == 0 || StartOptions->TtyRows == 0),
            "Invalid tty size: %lu:%lu",
            StartOptions->TtyRows,
            StartOptions->TtyColumns);
    }

    // Attach to the container's init process so no IO is lost.
    std::unique_ptr<WSLCProcessIO> io;

    try
    {
        if (WI_IsFlagSet(Flags, WSLCContainerStartFlagsAttach))
        {
            if (WI_IsFlagSet(m_initProcessFlags, WSLCProcessFlagsTty))
            {
                io = std::make_unique<TTYProcessIO>(TypedHandle{m_runtime.Docker().AttachContainer(m_id, detachKeys), WSLCHandleTypeSocket});
            }
            else
            {
                io = CreateRelayedProcessIO(wil::shared_socket{m_runtime.Docker().AttachContainer(m_id, detachKeys)}, m_initProcessFlags);
            }
        }
    }
    catch (const DockerHTTPException& e)
    {
        // N.B. This can happen if 'DetachKeys' is invalid.
        THROW_DOCKER_USER_ERROR_MSG(e, "Failed to attach to container '%hs' during start", m_id.c_str());
    }

    auto control = std::make_unique<DockerContainerProcessControl>(*this, m_runtime.Docker());

    {
        std::lock_guard processesLock{m_processesLock};
        m_initProcessControl = control.get();
        m_initProcess = wil::MakeOrThrow<WSLCProcess>(std::move(control), std::move(io), m_initProcessFlags);
    }

    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this]() mutable {
        std::lock_guard processesLock{m_processesLock};
        m_initProcess.Reset();
        m_initProcessControl = nullptr;
    });

    // Refuse to start if any referenced named volume is in a failed state.
    std::vector<std::string> unavailableVolumes;
    for (const auto& volumeName : m_namedVolumes)
    {
        const auto [code, message] = m_runtime.Volumes().GetVolumeStatus(volumeName);
        if (FAILED(code))
        {
            EMIT_USER_WARNING(Localization::MessageWslcVolumeNotAvailableReason(volumeName, message));
            unavailableVolumes.push_back(volumeName);
        }
    }

    THROW_HR_WITH_USER_ERROR_IF(
        WSLC_E_VOLUME_NOT_AVAILABLE,
        Localization::MessageWslcVolumeNotAvailable(wsl::shared::string::Join(unavailableVolumes, ',')),
        !unavailableVolumes.empty());

    auto volumeCleanup = MountVolumes(m_mountedVolumes, m_runtime.Vm());

    auto portCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this]() { UnmapPorts(); });
    MapPorts();

    try
    {
        m_runtime.Docker().StartContainer(m_id, detachKeys);
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to start container '%hs'", m_id.c_str());

    if (WI_IsFlagSet(m_initProcessFlags, WSLCProcessFlagsTty) && StartOptions != nullptr)
    {
        try
        {
            m_runtime.Docker().ResizeContainerTty(m_id, StartOptions->TtyRows, StartOptions->TtyColumns);
        }
        CATCH_LOG();
    }

    auto inspectJson = InspectLockHeld();
    const auto pluginResult = m_pluginNotifier->OnContainerStarted(inspectJson.c_str());
    if (FAILED(pluginResult))
    {
        // Forward the COM error message, if available.
        auto comError = wsl::windows::common::wslutil::GetCOMErrorInfo();

        LOG_HR_MSG(pluginResult, "Plugin rejected start of container '%hs' (0x%x)", m_id.c_str(), pluginResult);
        try
        {
            m_runtime.Docker().StopContainer(m_id.c_str(), {}, {});
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            EMIT_USER_WARNING(wsl::shared::Localization::MessageWslcContainerStopAfterPluginRejectionFailed(
                wsl::shared::string::MultiByteToWide(m_id)));
        }

        if (comError.has_value() && comError->Message)
        {
            THROW_HR_WITH_USER_ERROR(pluginResult, comError->Message.get());
        }
        else
        {
            THROW_HR(pluginResult);
        }
    }

    transition = StartTransition(TransitionKind::Start, ContainerEvent::Start);

    portCleanup.release();
    volumeCleanup.release();
    cleanup.release();

    lock.reset();
    lifecycleLock.reset();
    AttachToTransition(transition);
}

void WSLCContainerImpl::WaitForConflictingTransitionToComplete(
    wil::rwlock_release_exclusive_scope_exit& lock, wil::rwlock_release_shared_scope_exit& lifecycleLock, std::optional<TransitionKind> kind)
{
    while (m_transition && (!kind.has_value() || m_transition->Kind != kind.value()))
    {
        {
            auto transition = m_transition;
            lock.reset();
            lifecycleLock.reset();
            WaitForTransitionCompletion(transition);
        }

        lifecycleLock = m_lifecycleLock.lock_shared();
        lock = m_lock.lock_exclusive();
    }
}

__requires_exclusive_lock_held(m_lock) std::shared_ptr<WSLCContainerImpl::StateTransition> WSLCContainerImpl::StartTransition(
    TransitionKind kind, ContainerEvent expectedEvent)
{
    auto transition = std::make_shared<StateTransition>(kind, expectedEvent);
    WI_ASSERT(!m_transition);
    m_transition = transition;
    return transition;
}

void WSLCContainerImpl::WaitForTransitionCompletion(const std::shared_ptr<StateTransition>& transition) const
{
    auto io = m_wslcSession.CreateIOContext();
    io.AddHandle(std::make_unique<EventHandle>(transition->Completed.get()));
    io.Run({});

    WI_ASSERT(transition->Completed.is_signaled());
}

void WSLCContainerImpl::AttachToTransition(const std::shared_ptr<StateTransition>& transition) const
{
    WaitForTransitionCompletion(transition);

    unique_com_disconnect wrapper;

    // Take ownership of the deferred COM disconnect after OnEvent leaves its critical section.
    {
        auto lock = m_lock.lock_exclusive();
        wrapper = std::move(transition->Wrapper);
    }

    if (transition->Exception)
    {
        std::rethrow_exception(transition->Exception);
    }
}

__requires_exclusive_lock_held(m_lock) void WSLCContainerImpl::CompleteTransition(const std::shared_ptr<StateTransition>& transition, std::exception_ptr exception) noexcept
{
    WI_ASSERT(m_transition == transition);
    transition->Exception = std::move(exception);
    m_transition.reset();
    transition->Completed.SetEvent();
}

void WSLCContainerImpl::OnEvent(ContainerEvent event, std::optional<int> exitCode, std::int64_t eventTime) noexcept
{
    // Either owner may disconnect the COM wrapper, so both must outlive m_lock.
    unique_com_disconnect comWrapper;
    std::shared_ptr<StateTransition> transition;

    {
        auto lifecycleLock = m_lifecycleLock.lock_exclusive();
        auto lock = m_lock.lock_exclusive();
        transition = m_transition;

        if (event == ContainerEvent::Start)
        {
            // Only WSLC should start the container, so if we receive a start event, it must be expected by a transition.
            // Otherwise the container was started externally. Log if the container was started externally.
            if (transition && transition->ExpectedEvent == ContainerEvent::Start)
            {
                WI_ASSERT(m_state == WslcContainerStateCreated || m_state == WslcContainerStateExited);
                CommitState(WslcContainerStateRunning, eventTime);
                CompleteTransition(transition);
            }
            else
            {
                WSL_LOG("UnexpectedContainerStart", TraceLoggingValue(m_id.c_str(), "Id"));
            }
        }
        else if (event == ContainerEvent::Stop)
        {
            WI_ASSERT(exitCode.has_value());
            OnStopped(exitCode.value(), eventTime);
        }
        else if (event == ContainerEvent::Destroy)
        {
            if (m_state != WslcContainerStateDeleted)
            {
                CommitState(WslcContainerStateDeleted, eventTime);
                comWrapper = ReleaseResources();
            }

            // Signal init exit after the state transition and resource cleanup so awaiters observe Deleted.
            SignalInitProcessExit();

            if (transition)
            {
                WI_ASSERT(transition->ExpectedEvent == ContainerEvent::Destroy);

                // Let a COM caller waiting on this transition perform the disconnect, avoiding a deadlock with OnEvent.
                transition->Wrapper = std::move(comWrapper);

                CompleteTransition(transition);
            }
        }

        WSL_LOG(
            "ContainerEvent",
            TraceLoggingValue(m_name.c_str(), "Name"),
            TraceLoggingValue(m_id.c_str(), "Id"),
            TraceLoggingValue((int)event, "Event"));
    }
}

void WSLCContainerImpl::Stop(WSLCSignal Signal, LONG TimeoutSeconds, bool Kill)
{
    std::shared_ptr<StateTransition> transition;

    {
        auto lifecycleLock = m_lifecycleLock.lock_shared();
        auto lock = m_lock.lock_exclusive();
        WaitForConflictingTransitionToComplete(lock, lifecycleLock, TransitionKind::Stop);

        transition = m_transition;
        WI_ASSERT(!transition || transition->Kind == TransitionKind::Stop);

        // There can be an active stop transition post observing the exited state for cases where additional work needs to be done
        // after the container stopped: e.g. auto remove, restart, etc. Therefore, if there is an active stop transition, we still
        // need to attach to it below. This check simply skips creating a new transition once the state is already exited.
        if (!transition && m_state != WslcContainerStateRunning)
        {
            if (m_state == WslcContainerStateExited && !Kill)
            {
                return;
            }

            THROW_HR_WITH_USER_ERROR_MSG(
                WSLC_E_CONTAINER_NOT_RUNNING,
                Localization::MessageWslcContainerNotRunning(m_id),
                "Cannot stop container '%hs', state: %i",
                m_id.c_str(),
                m_state);
        }
        // This check ensures WSLC does not call into docker if it has already observed the exited state. This prevents
        // conflicting with scenarios where work needs to be done after the container exits.
        else if (m_state == WslcContainerStateRunning)
        {
            std::optional<WSLCSignal> SignalArg;

            if (Signal != WSLCSignalNone)
            {
                SignalArg = Signal;
            }

            ValidateStopTimeout(TimeoutSeconds, true);

            // Don't wait for the container to stop if we're not sending SIGKILL, since it may not stop the container.
            // N.B. If the signal was SIGTERM for instance, we'll receive the stop notification via OnEvent().
            bool waitForStop = !Kill || (SignalArg.value_or(WSLCSignalSIGKILL) == WSLCSignalSIGKILL);
            const auto generation = m_stateGeneration;

            lock.reset();
            lifecycleLock.reset();

            try
            {
                if (Kill)
                {
                    m_runtime.Docker().SignalContainer(m_id, SignalArg);
                }
                else
                {
                    std::optional<LONG> TimeoutArg;

                    if (TimeoutSeconds != WSLC_STOP_TIMEOUT_DEFAULT)
                    {
                        TimeoutArg = TimeoutSeconds;
                    }

                    m_runtime.Docker().StopContainer(m_id, SignalArg, TimeoutArg);
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

            if (waitForStop)
            {
                lock = m_lock.lock_exclusive();
                transition = m_transition;

                // The container can exit and start again while the locks are released, so an unchanged generation is
                // the only proof that the stop event this call is waiting for is still to come.
                if (m_stateGeneration == generation)
                {
                    if (!transition)
                    {
                        transition = StartTransition(TransitionKind::Stop, ContainerEvent::Stop);
                    }
                }
                // The run already ended: keep waiting on the work it triggered (e.g. auto-remove), never on a start
                // that raced in behind it.
                else if (transition && transition->Kind == TransitionKind::Start)
                {
                    transition.reset();
                }
            }
            else
            {
                transition.reset();
            }
        }
    }

    if (transition)
    {
        AttachToTransition(transition);
    }
}

__requires_exclusive_lock_held(m_lock) void WSLCContainerImpl::OnStopped(int exitCode, std::optional<std::int64_t> stopTimestamp)
{
    auto transition = m_transition;

    // A Stop while expecting Start should not occur normally: Docker emits start before die, and the event stream processes
    // them serially. It would indicate external manipulation. Ignoring it avoids applying an old exit code to the newly
    // staged init process.
    if (transition && (transition->ExpectedEvent == ContainerEvent::Start))
    {
        WSL_LOG("UnexpectedContainerExit", TraceLoggingValue(m_id.c_str(), "Id"), TraceLoggingValue(exitCode, "ExitCode"));
        return;
    }

    SetExitCode(exitCode);

    // Notify plugin manager that the container is stopping. Errors are ignored.
    if (m_state == WslcContainerStateRunning)
    {
        try
        {
            LOG_IF_FAILED(m_pluginNotifier->OnContainerStopping(m_id.c_str()));
        }
        CATCH_LOG();
    }

    ReleaseProcesses();
    ReleaseRuntimeResources();

    // Ignore duplicate or late Stop events so they do not overwrite an already committed state.
    if (m_state == WslcContainerStateRunning)
    {
        CommitState(WslcContainerStateExited, stopTimestamp);
    }

    std::exception_ptr transitionException;

    // Docker delete request is already sent.
    if (transition && transition->ExpectedEvent == ContainerEvent::Destroy)
    {
        return;
    }

    // Stop with Rm must initiate Delete.
    if (WI_IsFlagSet(m_containerFlags, WSLCContainerFlagsRm))
    {
        try
        {
            m_runtime.Docker().DeleteContainer(m_id, true, true);

            if (transition)
            {
                transition->ExpectedEvent = ContainerEvent::Destroy;
            }
            else
            {
                transition = StartTransition(TransitionKind::Delete, ContainerEvent::Destroy);
            }

            return;
        }
        catch (...)
        {
            transitionException = std::current_exception();
            LOG_CAUGHT_EXCEPTION_MSG("Failed to remove container '%hs'", m_id.c_str());
        }
    }

    SignalInitProcessExit();

    if (transition)
    {
        CompleteTransition(transition, std::move(transitionException));
    }
}

void WSLCContainerImpl::RecoverPorts(const common::docker_schema::ContainerInfo& dockerContainer)
{
    auto lock = m_lock.lock_exclusive();

    // Re-register VM-scoped port reservations against the restarted VM using the numbers recorded at
    // create time, restoring bridge-mode forwarding when the stopped container starts again.
    const bool allocateVmPorts = NetworkModeAllocatesVmPorts(m_networkMode);
    if (!allocateVmPorts)
    {
        return;
    }

    auto metadataIt = dockerContainer.Labels.find(WSLCContainerMetadataLabel);
    if (metadataIt == dockerContainer.Labels.end())
    {
        return;
    }

    auto metadata = ParseContainerMetadata(metadataIt->second.c_str());

    std::vector<ContainerPortMapping> ports;
    ports.reserve(metadata.Ports.size());
    for (const auto& e : metadata.Ports)
    {
        auto& inserted = ports.emplace_back(ContainerPortMapping{VMPortMapping::FromContainerMetaData(e), e.ContainerPort});

        auto allocation = m_runtime.Vm().TryAllocatePort(e.VmPort, e.Family, e.Protocol);

        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), !allocation, "Port %hu is in use, cannot recover container %hs", e.VmPort, m_id.c_str());

        inserted.VmMapping.AssignVmPort(allocation);
    }

    m_mappedPorts = std::move(ports);
}

void WSLCContainerImpl::Delete(WSLCDeleteFlags Flags)
{
    std::shared_ptr<StateTransition> transition;
    auto lifecycleLock = m_lifecycleLock.lock_shared();
    auto lock = m_lock.lock_exclusive();
    WaitForConflictingTransitionToComplete(lock, lifecycleLock);

    RequestDeleteExclusiveLockHeld(Flags);
    transition = StartTransition(TransitionKind::Delete, ContainerEvent::Destroy);

    lock.reset();
    lifecycleLock.reset();
    AttachToTransition(transition);
}

__requires_exclusive_lock_held(m_lock) void WSLCContainerImpl::RequestDeleteExclusiveLockHeld(WSLCDeleteFlags Flags)
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
        m_runtime.Docker().DeleteContainer(m_id, WI_IsFlagSet(Flags, WSLCDeleteFlagsForce), WI_IsFlagSet(Flags, WSLCDeleteFlagsDeleteVolumes));
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to delete container '%hs'", m_id.c_str());
}

void WSLCContainerImpl::Export(WSLCHandle OutHandle) const
{
    auto lock = m_lock.lock_shared();

    // Validate that the container is not in the running state.
    THROW_HR_WITH_USER_ERROR_IF(WSLC_E_CONTAINER_IS_RUNNING, Localization::MessageWslcContainerIsRunning(m_id), m_state == WslcContainerStateRunning);

    std::pair<uint32_t, wil::unique_socket> SocketCodePair;
    SocketCodePair = m_runtime.Docker().ExportContainer(m_id);

    auto userHandle = m_wslcSession.OpenUserHandle(OutHandle);

    wsl::windows::common::io::MultiHandleWait io = m_wslcSession.CreateIOContext();

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
        io.AddHandle(std::make_unique<RelayHandle<HTTPChunkBasedReadHandle>>(
            HandleWrapper{std::move(SocketCodePair.second)}, userHandle.Get()));
    }

    // Release the lock so the container can still be interacted with while the export is in progress.
    // Past this point, no member variables can be accessed.
    lock.reset();

    io.Run({});

    if (SocketCodePair.first != 200)
    {
        // Export failed, parse the error message.
        auto error = wsl::shared::FromJson<common::docker_schema::ErrorResponse>(errorJson.c_str());
        const auto errorMessage = FormatDockerEngineError(error.message);

        THROW_HR_WITH_USER_ERROR_IF(WSLC_E_CONTAINER_NOT_FOUND, errorMessage, SocketCodePair.first == 404);
        THROW_HR_WITH_USER_ERROR(E_FAIL, errorMessage);
    }
}

void WSLCContainerImpl::UploadArchive(WSLCHandle TarHandle, LPCSTR DestPath, ULONGLONG ContentSize) const
{
    auto lock = m_lock.lock_shared();

    std::optional<uint64_t> contentLength;
    if (ContentSize > 0)
    {
        contentLength = ContentSize;
    }

    auto requestContext = m_runtime.Docker().PutArchive(m_id, DestPath, contentLength);

    auto userHandle = m_wslcSession.OpenUserHandle(TarHandle);

    auto io = m_wslcSession.CreateIOContext();

    std::optional<std::string> pendingErrorJson;
    unsigned int httpStatusCode = 0;
    auto onHttpResponse = [&](const boost::beast::http::message<false, boost::beast::http::buffer_body>& response) {
        WSL_LOG("ContainerUploadArchiveHttpResponse", TraceLoggingValue(static_cast<int>(response.result()), "StatusCode"));

        httpStatusCode = response.result_int();
        if (httpStatusCode != 200)
        {
            pendingErrorJson.emplace();
        }
    };

    auto onProgress = [&](const gsl::span<char>& buffer) {
        if (pendingErrorJson.has_value())
        {
            pendingErrorJson->append(buffer.data(), buffer.size());
        }
    };

    // Shutdown the Docker stream's write side when the input is fully read.
    auto onInputComplete = [socket = requestContext->stream.native_handle()]() {
        LOG_LAST_ERROR_IF(shutdown(socket, SD_SEND) == SOCKET_ERROR);
    };

    io.AddHandle(std::make_unique<RelayHandle<ReadHandle>>(
        HandleWrapper{userHandle.Get(), std::move(onInputComplete)}, HandleWrapper{requestContext->stream.native_handle()}));

    io.AddHandle(
        std::make_unique<DockerHTTPClient::DockerHttpResponseHandle>(*requestContext, std::move(onHttpResponse), std::move(onProgress)),
        wsl::windows::common::io::MultiHandleWait::CancelOnCompleted);

    // Release the lock so the container can still be interacted with while the upload is in progress.
    lock.reset();

    io.Run({});

    if (pendingErrorJson.has_value())
    {
        auto error = wsl::shared::FromJson<ErrorResponse>(pendingErrorJson->c_str());
        const auto errorMessage = FormatDockerEngineError(error.message);

        THROW_HR_WITH_USER_ERROR_IF(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), errorMessage, httpStatusCode == 404);
        THROW_HR_WITH_USER_ERROR(E_FAIL, errorMessage);
    }
}

void WSLCContainerImpl::DownloadArchive(LPCSTR SrcPath, WSLCHandle OutHandle) const
{
    auto lock = m_lock.lock_shared();

    auto [statusCode, socket, isChunked] = m_runtime.Docker().GetArchive(m_id, SrcPath);

    auto userHandle = m_wslcSession.OpenUserHandle(OutHandle);

    wsl::windows::common::io::MultiHandleWait io = m_wslcSession.CreateIOContext();

    std::string errorJson;

    if (statusCode != 200)
    {
        io.AddHandle(std::make_unique<ReadHandle>(HandleWrapper{std::move(socket)}, [&](const gsl::span<char>& buffer) {
            errorJson.append(buffer.data(), buffer.size());
        }));
    }
    else
    {
        if (isChunked)
        {
            io.AddHandle(
                std::make_unique<RelayHandle<HTTPChunkBasedReadHandle>>(HandleWrapper{std::move(socket)}, userHandle.Get()),
                wsl::windows::common::io::MultiHandleWait::CancelOnCompleted);
        }
        else
        {
            io.AddHandle(
                std::make_unique<RelayHandle<ReadHandle>>(HandleWrapper{std::move(socket)}, userHandle.Get()),
                wsl::windows::common::io::MultiHandleWait::CancelOnCompleted);
        }
    }

    lock.reset();

    io.Run({});

    if (statusCode != 200)
    {
        auto error = wsl::shared::FromJson<ErrorResponse>(errorJson.c_str());
        const auto errorMessage = FormatDockerEngineError(error.message);

        THROW_HR_WITH_USER_ERROR_IF(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), errorMessage, statusCode == 404);
        THROW_HR_WITH_USER_ERROR(E_FAIL, errorMessage);
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

void WSLCContainerImpl::Exec(const WSLCProcessOptions* Options, const WSLCProcessStartOptions* StartOptions, IWSLCProcess** Process)
{
    THROW_HR_IF_MSG(E_INVALIDARG, Options->CommandLine.Count == 0, "Exec command line cannot be empty");

    auto lock = m_lock.lock_shared();

    THROW_HR_WITH_USER_ERROR_IF(WSLC_E_CONTAINER_NOT_RUNNING, Localization::MessageWslcContainerNotRunning(m_id), m_state != WslcContainerStateRunning);

    if (StartOptions != nullptr)
    {
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            WI_IsFlagSet(Options->Flags, WSLCProcessFlagsTty) && (StartOptions->TtyRows == 0 || StartOptions->TtyColumns == 0),
            "Invalid tty size: %lu:%lu",
            StartOptions->TtyRows,
            StartOptions->TtyColumns);
    }

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

        if (StartOptions != nullptr)
        {
            request.ConsoleSize = {StartOptions->TtyRows, StartOptions->TtyColumns};
        }
    }

    if (WI_IsFlagSet(Options->Flags, WSLCProcessFlagsStdin))
    {
        request.AttachStdin = true;
    }

    if (StartOptions != nullptr && StartOptions->DetachKeys != nullptr)
    {
        request.DetachKeys = StartOptions->DetachKeys;
    }

    try
    {
        auto result = m_runtime.Docker().CreateExec(m_id, request);

        // N.B. There's no way to delete a created exec instance, it is removed when the container is deleted.

        auto stream = m_runtime.Docker().StartExec(
            result.Id, common::docker_schema::StartExec{.Tty = request.Tty, .ConsoleSize = request.ConsoleSize});

        std::unique_ptr<WSLCProcessIO> io;
        if (request.Tty)
        {
            io = std::make_unique<TTYProcessIO>(TypedHandle{std::move(stream), WSLCHandleTypeSocket});
        }
        else
        {
            io = CreateRelayedProcessIO(wil::shared_socket{std::move(stream)}, Options->Flags);
        }

        auto control = std::make_shared<DockerExecProcessControl>(*this, result.Id, m_runtime.Docker(), m_runtime.Events());

        {
            std::lock_guard processesLock{m_processesLock};

            // Drop entries for execs that have since been released, then store a non-owning weak
            // reference. The owning shared_ptr is moved into the COM WSLCProcess returned below.
            std::erase_if(m_processes, [](const auto& weak) { return weak.expired(); });
            m_processes.push_back(control);
        }

        // Poll for the exec'd process to either be running, or failed.
        // This is required because StartExec() returns before the process is actually created, and if exec() fails, we'll never
        // get an exec_die notification, so this case needs to be caught before returning the process to the caller.
        //
        // N.B. Pid is 0 until runc forks the user process, so a transient {Running=true, Pid=0} response (seen e.g. on a
        // fast failure such as an invalid user/group) must not be treated as "running" or we'd wait forever.

        // TODO: Configurable timeout.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

        do
        {
            auto state = m_runtime.Docker().InspectExec(result.Id);
            if (state.Running && state.Pid > 0)
            {
                control->SetPid(state.Pid);
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

        // The exec'd process wrapper is handed to the client and is not retained internally, so its
        // lifetime tracks the client's proxy. Bind a keep-alive token to it so the idle worker does
        // not tear the VM down (killing the process) while the client still holds the proxy.
        process->SetKeepAliveToken(m_wslcSession.CreateActivityToken());

        THROW_IF_FAILED(process.CopyTo(__uuidof(IWSLCProcess), (void**)Process));
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to exec process in container %hs", m_id.c_str());
}

WslcInspectContainer WSLCContainerImpl::BuildInspectContainer(const DockerInspectContainer& dockerInspect) const
{
    WslcInspectContainer wslcInspect{};

    wslcInspect.Id = dockerInspect.Id;
    wslcInspect.Name = dockerInspect.Name;
    wslcInspect.Created = dockerInspect.Created;
    wslcInspect.Image = dockerInspect.Image;

    // Map container state.
    wslcInspect.State.Status = dockerInspect.State.Status;
    wslcInspect.State.Running = dockerInspect.State.Running;
    wslcInspect.State.ExitCode = dockerInspect.State.ExitCode;
    wslcInspect.State.StartedAt = dockerInspect.State.StartedAt;
    wslcInspect.State.FinishedAt = dockerInspect.State.FinishedAt;

    if (dockerInspect.State.Health.has_value())
    {
        const auto& dockerHealth = dockerInspect.State.Health.value();

        wslc_schema::Health health{};
        health.Status = dockerHealth.Status;
        health.FailingStreak = dockerHealth.FailingStreak;

        health.Log.reserve(dockerHealth.Log.size());
        for (const auto& entry : dockerHealth.Log)
        {
            health.Log.push_back({entry.Start, entry.End, entry.ExitCode, entry.Output});
        }

        wslcInspect.State.Health = std::move(health);
    }

    wslcInspect.HostConfig.NetworkMode = dockerInspect.HostConfig.NetworkMode;
    wslcInspect.HostConfig.Memory = dockerInspect.HostConfig.Memory;
    wslcInspect.HostConfig.NanoCpus = dockerInspect.HostConfig.NanoCpus;

    if (dockerInspect.HostConfig.Ulimits.has_value())
    {
        wslcInspect.HostConfig.Ulimits.reserve(dockerInspect.HostConfig.Ulimits->size());
        for (const auto& ulimit : dockerInspect.HostConfig.Ulimits.value())
        {
            wslcInspect.HostConfig.Ulimits.push_back({ulimit.Name, ulimit.Soft, ulimit.Hard});
        }
    }

    wslcInspect.Config.Image = m_image;
    wslcInspect.Config.Env = dockerInspect.Config.Env;
    wslcInspect.Config.Cmd = dockerInspect.Config.Cmd;
    wslcInspect.Config.Entrypoint = dockerInspect.Config.Entrypoint;
    wslcInspect.Config.User = dockerInspect.Config.User;
    wslcInspect.Config.WorkingDir = dockerInspect.Config.WorkingDir;
    wslcInspect.Config.StopTimeout = dockerInspect.Config.StopTimeout;

    if (dockerInspect.Config.Healthcheck.has_value())
    {
        const auto& dockerHealth = dockerInspect.Config.Healthcheck.value();

        wslc_schema::HealthConfig health{};
        health.Test = dockerHealth.Test;
        health.Interval = dockerHealth.Interval;
        health.Timeout = dockerHealth.Timeout;
        health.StartPeriod = dockerHealth.StartPeriod;
        health.Retries = dockerHealth.Retries;

        wslcInspect.Config.Healthcheck = std::move(health);
    }

    // Map WSLC port mappings (Windows host ports only).
    for (const auto& e : m_mappedPorts)
    {
        auto portKey = std::format("{}/{}", e.ContainerPort, e.ProtocolString());

        wslc_schema::InspectPortBinding portBinding{};
        portBinding.HostIp = e.VmMapping.BindingAddressString();
        portBinding.HostPort = std::to_string(e.VmMapping.HostPort());

        wslcInspect.Ports[portKey].push_back(std::move(portBinding));
    }

    // Map mounts without exposing Linux paths from the utility VM.
    wslcInspect.Mounts.reserve(
        m_mountedVolumes.size() + dockerInspect.Mounts.size() + dockerInspect.HostConfig.Tmpfs.size() +
        dockerInspect.HostConfig.Mounts.size());
    for (const auto& volume : m_mountedVolumes)
    {
        wslc_schema::InspectMount mountInfo{};
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

    for (const auto& volume : dockerInspect.Mounts)
    {
        // This block covers non-vhd volumes. This includes:
        // - Guest volumes mounted via -v
        // - Volumes mounted as part of the image (via VOLUME)
        //
        // TODO: Return mounts once --mount is implemented.

        if (volume.Type != "volume")
        {
            continue;
        }

        wslc_schema::InspectMount mountInfo{};
        mountInfo.Type = volume.Type;
        mountInfo.Name = volume.Name;
        const auto structuredMount = std::ranges::find_if(dockerInspect.HostConfig.Mounts, [&](const auto& mount) {
            return mount.Type == "volume" && mount.Target == volume.Destination;
        });
        if (structuredMount != dockerInspect.HostConfig.Mounts.end())
        {
            mountInfo.Source = structuredMount->Source;
        }
        mountInfo.Destination = volume.Destination;
        mountInfo.ReadWrite = volume.RW;

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

    // Bind mounts are populated from m_mountedVolumes so their inspect source is the Windows host path.
    for (const auto& mount : dockerInspect.HostConfig.Mounts)
    {
        if (mount.Type == "tmpfs")
        {
            wslc_schema::InspectMount mountInfo{};
            mountInfo.Type = mount.Type;
            mountInfo.Source = mount.Source;
            mountInfo.Destination = mount.Target;
            mountInfo.ReadWrite = !mount.ReadOnly;
            wslcInspect.Mounts.push_back(std::move(mountInfo));
        }
    }

    // Config.Labels is the Docker-shape location; top-level Labels is a legacy alias.
    wslcInspect.Config.Labels = m_labels;
    wslcInspect.Labels = m_labels;

    // Map per-endpoint network settings from Docker inspect data.
    for (const auto& [name, endpoint] : dockerInspect.NetworkSettings.Networks)
    {
        wslc_schema::InspectEndpointSettings wslcEndpoint{};
        wslcEndpoint.IPAddress = endpoint.IPAddress;
        wslcEndpoint.Gateway = endpoint.Gateway;
        wslcEndpoint.MacAddress = endpoint.MacAddress;
        wslcEndpoint.IPPrefixLen = endpoint.IPPrefixLen;
        wslcEndpoint.Aliases = endpoint.Aliases.value_or(std::vector<std::string>{});
        wslcEndpoint.Links = endpoint.Links.value_or(std::vector<std::string>{});
        wslcEndpoint.DriverOpts = endpoint.DriverOpts.value_or(std::map<std::string, std::string>{});
        if (endpoint.IPAMConfig.has_value())
        {
            wslc_schema::InspectEndpointIPAMConfig ipam{};
            ipam.IPv4Address = endpoint.IPAMConfig->IPv4Address;
            ipam.LinkLocalIPs = endpoint.IPAMConfig->LinkLocalIPs.value_or(std::vector<std::string>{});
            wslcEndpoint.IPAMConfig = std::move(ipam);
        }
        wslcInspect.NetworkSettings.Networks[name] = std::move(wslcEndpoint);
    }

    return wslcInspect;
}

std::shared_ptr<WSLCContainerImpl> WSLCContainerImpl::Create(
    const WSLCContainerOptions& containerOptions,
    const std::string& containerName,
    WSLCSession& wslcSession,
    WSLCSessionRuntime& runtime,
    IWSLCPluginNotifier* pluginNotifier,
    const std::unordered_map<std::string, NetworkEntry>& sessionNetworks,
    std::function<void(const WSLCContainerImpl*)>&& OnDeleted)
{
    auto& virtualMachine = runtime.Vm();
    auto& DockerClient = runtime.Docker();
    auto& EventTracker = runtime.Events();
    const auto mounts = ConvertAndValidateMounts(containerOptions);

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

    if (WI_IsFlagSet(containerOptions.Flags, WSLCContainerFlagsStopTimeout))
    {
        ValidateStopTimeout(containerOptions.StopTimeout, false);

        request.StopTimeout = static_cast<int>(containerOptions.StopTimeout);
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

    request.HostConfig.Memory = containerOptions.MemoryBytes;
    request.HostConfig.NanoCpus = containerOptions.NanoCpus;

    if (containerOptions.UlimitsCount > 0)
    {
        THROW_HR_IF_NULL_MSG(E_INVALIDARG, containerOptions.Ulimits, "Ulimits is null with UlimitsCount=%lu", containerOptions.UlimitsCount);

        std::vector<wsl::windows::common::docker_schema::Ulimit> ulimits;
        ulimits.reserve(containerOptions.UlimitsCount);

        for (ULONG i = 0; i < containerOptions.UlimitsCount; i++)
        {
            const auto& ulimit = containerOptions.Ulimits[i];
            THROW_HR_IF_NULL_MSG(E_INVALIDARG, ulimit.Name, "Ulimits[%lu].Name is null", i);

            ulimits.push_back({ulimit.Name, ulimit.Soft, ulimit.Hard});
        }

        request.HostConfig.Ulimits = std::move(ulimits);
    }

    request.HostConfig.ShmSize = containerOptions.ShmSize;

    if (WI_IsFlagSet(containerOptions.Flags, WSLCContainerFlagsNoHealthCheck))
    {
        THROW_HR_IF_MSG(
            E_INVALIDARG,
            WI_IsFlagSet(containerOptions.Flags, WSLCContainerFlagsHealthCheck),
            "WSLCContainerFlagsHealthCheck and WSLCContainerFlagsNoHealthCheck cannot be combined");

        request.Healthcheck.emplace().Test = std::vector<std::string>{"NONE"};
    }
    else if (WI_IsFlagSet(containerOptions.Flags, WSLCContainerFlagsHealthCheck))
    {
        common::docker_schema::HealthConfig health{};

        if (containerOptions.HealthCmd != nullptr)
        {
            health.Test = std::vector<std::string>{"CMD-SHELL", containerOptions.HealthCmd};
        }

        // N.B. '0' will use the default value from the image.
        if (containerOptions.HealthIntervalNs != 0)
        {
            health.Interval = containerOptions.HealthIntervalNs;
        }

        if (containerOptions.HealthTimeoutNs != 0)
        {
            health.Timeout = containerOptions.HealthTimeoutNs;
        }

        if (containerOptions.HealthStartPeriodNs != 0)
        {
            health.StartPeriod = containerOptions.HealthStartPeriodNs;
        }

        if (containerOptions.HealthRetries != 0)
        {
            health.Retries = containerOptions.HealthRetries;
        }

        request.Healthcheck = std::move(health);
    }

    // Build bind mount list from container options.
    std::vector<WSLCVolumeMount> volumes;
    volumes.reserve(containerOptions.VolumesCount + mounts.size());

    std::vector<std::string> binds;
    binds.reserve(containerOptions.VolumesCount);

    for (ULONG i = 0; i < containerOptions.VolumesCount; i++)
    {
        auto volume = containerOptions.Volumes[i];
        auto prepared =
            PrepareBindMount(volume.HostPath, volume.ContainerPath, static_cast<bool>(volume.ReadOnly), MissingBindSource::Create);
        binds.push_back(std::format("{}:{}:{}", prepared.DockerSource, volume.ContainerPath, volume.ReadOnly ? "ro" : "rw"));
        volumes.push_back(std::move(prepared.Volume));
    }

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

    ProcessNamedVolumes(containerOptions, request);

    for (const auto& mount : mounts)
    {
        common::docker_schema::Mount dockerMount{
            .Target = mount.Target,
            .ReadOnly = mount.ReadOnly,
        };

        switch (mount.MountType)
        {
        case WSLCMountTypeBind:
        {
            // Docker's colon-delimited bind format cannot represent ':' in the target.
            const auto missingSource = mount.BindSource == wsl::windows::common::mount::BindSourcePolicy::CreateIfMissing
                                           ? MissingBindSource::Create
                                           : MissingBindSource::Reject;
            auto prepared = PrepareBindMount(mount.Source, mount.Target, mount.ReadOnly, missingSource);
            dockerMount.Source = std::move(prepared.DockerSource);
            dockerMount.Type = "bind";
            volumes.push_back(std::move(prepared.Volume));
            break;
        }

        case WSLCMountTypeVolume:
            dockerMount.Source = wsl::shared::string::WideToMultiByte(mount.Source);
            dockerMount.Type = "volume";
            break;

        case WSLCMountTypeTmpfs:
            if (mount.TmpfsOptions.has_value())
            {
                request.HostConfig.Tmpfs[mount.Target] = mount.TmpfsOptions.value();
                continue;
            }

            dockerMount.Type = "tmpfs";
            if (mount.TmpfsSizeBytes.has_value() || mount.TmpfsMode.has_value())
            {
                dockerMount.TmpfsOptions = common::docker_schema::MountTmpfsOptions{
                    .SizeBytes = mount.TmpfsSizeBytes.value_or(0),
                    .Mode = mount.TmpfsMode.value_or(0),
                };
            }
            break;
        }

        request.HostConfig.Mounts.push_back(std::move(dockerMount));
    }

    request.HostConfig.Binds = std::move(binds);

    // Configure GPU support if requested.
    if (WI_IsFlagSet(containerOptions.Flags, WSLCContainerFlagsGpu))
    {
        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
            !virtualMachine.FeatureEnabled(WslcFeatureFlagsGPU),
            "WSLCContainerFlagsGpu requires GPU support enabled on the session");

        // Request the WSL GPU device via CDI.
        request.HostConfig.DeviceRequests = std::vector<common::docker_schema::DeviceRequest>{{"cdi", {LX_WSLC_GPU_CDI_DEVICE}}};
    }

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
            // The userspace wslrelay port relay only forwards TCP. When it's active, adding a UDP
            // mapping would fail the whole container (MapPort throws ERROR_NOT_SUPPORTED), so skip
            // UDP exposed ports and warn once. UDP is published normally on the virtioNet path.
            const bool relayForwarding = virtualMachine.UseWslRelayPortForwarding();
            bool warnedUdpSkipped = false;

            for (const auto& [portKey, _] : imageInfo.Config->ExposedPorts.value())
            {
                auto [port, protocol] = ParseExposedPortKey(portKey);

                if (relayForwarding && protocol == IPPROTO_UDP)
                {
                    if (!warnedUdpSkipped)
                    {
                        EMIT_USER_WARNING(Localization::MessageWslcPublishAllUdpNotSupported());
                        warnedUdpSkipped = true;
                    }

                    continue;
                }

                // Exposed ports carry only a port and protocol (tcp/udp), never an address family.
                // Mirror Docker's dual-stack default by publishing each exposed port on both the IPv4
                // and IPv6 loopback, while keeping wslc's loopback-only default binding convention.
                for (const auto& [family, address] : {std::pair{AF_INET, "127.0.0.1"}, std::pair{AF_INET6, "::1"}})
                {
                    auto& createdPort = ports.emplace_back();
                    createdPort.HostPort = WSLC_EPHEMERAL_PORT;
                    createdPort.Family = family;
                    createdPort.ContainerPort = port;
                    createdPort.Protocol = protocol;
                    strcpy_s(createdPort.BindingAddress, address);
                }
            }
        }
    }

    auto networkMode = ResolveNetworkMode(containerOptions.ContainerNetwork.NetworkMode, !ports.empty(), sessionNetworks, DockerClient);

    auto endpoints = ResolveEndpoints(
        containerOptions.ContainerNetwork.Networks, containerOptions.ContainerNetwork.NetworksCount, networkMode, sessionNetworks);

    auto primaryConfig =
        ResolveEndpointConfig(containerOptions.ContainerNetwork.Settings, containerOptions.ContainerNetwork.SettingsCount, networkMode);

    THROW_HR_WITH_USER_ERROR_IF(
        E_INVALIDARG,
        Localization::MessageWslcAliasRequiresUserDefinedNetwork(),
        primaryConfig.Aliases.has_value() && !NetworkSupportsAliases(networkMode));

    const bool hasNonAliasEndpointSettings =
        primaryConfig.IPAMConfig.has_value() || primaryConfig.Links.has_value() || primaryConfig.DriverOpts.has_value();
    // N.B. NetworkModeAllocatesVmPorts is reused here as the "supports endpoint settings" predicate: modes
    // that lack a dedicated netns (host/none/container:*) also can't accept per-endpoint settings.
    THROW_HR_WITH_USER_ERROR_IF(
        E_INVALIDARG,
        Localization::MessageWslcEndpointSettingsRequireNetwork(networkMode),
        hasNonAliasEndpointSettings && !NetworkModeAllocatesVmPorts(networkMode));

    auto mappedPorts = BuildPortMappings(ports, networkMode, virtualMachine);

    request.HostConfig.NetworkMode = networkMode;
    request.NetworkingConfig.EndpointsConfig = std::move(endpoints);

    const bool hasPrimaryEndpointSettings = primaryConfig.Aliases.has_value() || primaryConfig.IPAMConfig.has_value() ||
                                            primaryConfig.Links.has_value() || primaryConfig.DriverOpts.has_value();
    if (NetworkModeAllocatesVmPorts(networkMode) && (!request.NetworkingConfig.EndpointsConfig.empty() || hasPrimaryEndpointSettings))
    {
        request.NetworkingConfig.EndpointsConfig[networkMode] = std::move(primaryConfig);
    }

    for (const auto& e : mappedPorts)
    {
        auto portKey = std::format("{}/{}", e.ContainerPort, e.ProtocolString());
        request.ExposedPorts[portKey] = {};

        auto& portEntry = request.HostConfig.PortBindings[portKey];

        // In host mode, VmPort is empty until the container starts.
        // In that networking mode, the host port always matches the vm port.
        auto hostPort = e.VmMapping.VmPort ? e.VmMapping.VmPort->Port() : e.VmMapping.HostPort();

        // Use catch-all binding address based on the address family. :: binds all ipv6 interfaces, and 0:0:0:0 binds all ipv4 interfaces.
        portEntry.emplace_back(common::docker_schema::PortMapping{
            .HostIp = e.VmMapping.IsIPv6() ? "::" : "0.0.0.0", .HostPort = std::to_string(hostPort)});
    }

    auto requestedLabels = ParseKeyValuePairs(containerOptions.Labels, containerOptions.LabelsCount, WSLCContainerMetadataLabel);

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
    request.Labels.insert(requestedLabels.begin(), requestedLabels.end());

    // Docker validates structured bind sources during container creation, so their VM paths must exist here.
    // Release the temporary shares before returning; Start remounts them for the container lifetime.
    auto result = [&]() {
        auto volumeCleanup = MountVolumes(volumes, virtualMachine);
        return DockerClient.CreateContainer(request, containerName);
    }();

    // Surface any warnings returned by Docker (e.g., deprecated features, configuration issues).
    for (const auto& warning : result.Warnings)
    {
        EMIT_USER_WARNING(wsl::shared::string::MultiByteToWide(warning));
    }

    // Clean up the Docker container if anything below fails.
    // N.B. The container ID is captured by value since it is moved into the WSLCContainerImpl constructor below.
    auto deleteOnFailure = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&DockerClient, containerId = result.Id]() {
        DockerClient.DeleteContainer(containerId, true, true);
    });

    // Inspect the container to fetch its generated name (if needed) and Docker's authoritative Created timestamp.
    auto inspectData = DockerClient.InspectContainer(result.Id);

    // Post-create verification: confirm every requested network is actually attached.
    // If Docker rejected any endpoint, throw here so deleteOnFailure cleans up the orphan container.
    // container:<id> mode shares the target's netns, so the mode string is not a network name and
    // won't appear in NetworkSettings.Networks. Skip the check for that mode.
    if (!networkMode.starts_with(c_containerNetworkPrefix))
    {
        THROW_HR_IF_MSG(
            E_UNEXPECTED,
            !inspectData.NetworkSettings.Networks.contains(networkMode),
            "Container was created but primary network '%hs' was not attached",
            networkMode.c_str());
    }

    for (const auto& [name, _] : request.NetworkingConfig.EndpointsConfig)
    {
        // The primary network was auto-inserted into EndpointsConfig for Docker v1.44 compat
        // and is already verified above. Only check explicitly-requested additional networks here.
        if (name == networkMode)
        {
            continue;
        }

        THROW_HR_IF_MSG(
            E_UNEXPECTED,
            !inspectData.NetworkSettings.Networks.contains(name),
            "Container was created but requested network '%hs' was not attached",
            name.c_str());
    }

    // Wait for the container create event to be delivered on the Docker event stream so that
    // any events for objects created for the container (e.g. volumes) are delivered before we return
    // from this function.
    EventTracker.WaitForObjectCreated(result.Id);

    // Collect the names of referenced docker named volumes so Start() can verify
    // they are available before running the container.
    std::vector<std::string> namedVolumes;
    namedVolumes.reserve(containerOptions.NamedVolumesCount + mounts.size());
    for (ULONG i = 0; i < containerOptions.NamedVolumesCount; i++)
    {
        namedVolumes.emplace_back(containerOptions.NamedVolumes[i].Name);
    }

    for (const auto& mount : mounts)
    {
        if (mount.MountType == WSLCMountTypeVolume && !mount.Source.empty())
        {
            namedVolumes.emplace_back(wsl::shared::string::WideToMultiByte(mount.Source));
        }
    }

    auto mergedLabels = StripInternalLabels(std::move(inspectData.Config.Labels));

    auto container = std::make_shared<WSLCContainerImpl>(
        wslcSession,
        runtime,
        pluginNotifier,
        std::move(result.Id),
        CleanContainerName(inspectData.Name),
        std::string(containerOptions.Image),
        std::move(networkMode),
        std::move(volumes),
        std::move(namedVolumes),
        std::move(mappedPorts),
        std::move(mergedLabels),
        std::move(OnDeleted),
        WslcContainerStateCreated,
        wsl::windows::common::timestamp::Rfc3339ToEpoch(inspectData.Created),
        containerOptions.InitProcessOptions.Flags,
        containerOptions.Flags);

    container->Initialize();

    deleteOnFailure.release();
    return container;
}

std::shared_ptr<WSLCContainerImpl> WSLCContainerImpl::Open(
    const common::docker_schema::ContainerInfo& dockerContainer,
    WSLCSession& wslcSession,
    WSLCSessionRuntime& runtime,
    IWSLCPluginNotifier* pluginNotifier,
    std::function<void(const WSLCContainerImpl*)>&& OnDeleted)
{
    auto& virtualMachine = runtime.Vm();
    auto& DockerClient = runtime.Docker();

    // Extract container name from Docker's names list.
    std::string name = ExtractContainerName(dockerContainer.Names, dockerContainer.Id);

    // Collect the names of referenced docker named volumes.
    std::vector<std::string> namedVolumes;
    for (const auto& mount : dockerContainer.Mounts)
    {
        if (mount.Type == "volume" && !mount.Name.empty())
        {
            namedVolumes.push_back(mount.Name);
        }
    }

    auto metadataIt = dockerContainer.Labels.find(WSLCContainerMetadataLabel);

    THROW_HR_IF_MSG(
        E_INVALIDARG,
        metadataIt == dockerContainer.Labels.end(),
        "Cannot open WSLC container %hs: missing WSLC metadata label",
        dockerContainer.Id.c_str());

    WI_ASSERT(dockerContainer.State != ContainerState::Running);

    auto metadata = ParseContainerMetadata(metadataIt->second.c_str());
    auto labels = StripInternalLabels(dockerContainer.Labels);

    // Docker treats empty NetworkMode as the default (bridge).
    std::string networkMode = dockerContainer.HostConfig.NetworkMode.empty() ? std::string{"bridge"} : dockerContainer.HostConfig.NetworkMode;

    RejectUnsupportedNetworkModes(networkMode);

    const bool allocateVmPorts = NetworkModeAllocatesVmPorts(networkMode);

    // Re-register recovered VM ports in the allocation pool to prevent conflicts.
    std::vector<ContainerPortMapping> ports;
    for (const auto& e : metadata.Ports)
    {
        auto& inserted = ports.emplace_back(ContainerPortMapping{VMPortMapping::FromContainerMetaData(e), e.ContainerPort});

        if (allocateVmPorts)
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

    auto container = std::make_shared<WSLCContainerImpl>(
        wslcSession,
        runtime,
        pluginNotifier,
        std::string(dockerContainer.Id),
        std::move(name),
        std::string(dockerContainer.Image),
        std::move(networkMode),
        std::move(metadata.Volumes),
        std::move(namedVolumes),
        std::move(ports),
        std::move(labels),
        std::move(OnDeleted),
        DockerStateToWSLCState(dockerContainer.State),
        dockerContainer.Created,
        metadata.InitProcessFlags,
        metadata.Flags);

    container->Initialize();

    // Restore the state change timestamp from Docker inspect data.
    try
    {
        auto inspectData = DockerClient.InspectContainer(dockerContainer.Id);
        auto state = DockerStateToWSLCState(dockerContainer.State);

        if (state == WslcContainerStateCreated)
        {
            // A created-but-never-started container has no StartedAt/FinishedAt; its state last
            // changed when it was created.
            container->m_stateChangedAt = dockerContainer.Created;
        }
        else
        {
            const auto& timestamp = (state == WslcContainerStateRunning) ? inspectData.State.StartedAt : inspectData.State.FinishedAt;

            if (!timestamp.empty() && timestamp != c_unsetTimestamp)
            {
                container->m_stateChangedAt = wsl::windows::common::timestamp::Rfc3339ToEpoch(timestamp);
            }
        }
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        EMIT_USER_WARNING(wsl::shared::Localization::MessageWslcContainerTimestampRecoveryFailed(
            wsl::shared::string::MultiByteToWide(dockerContainer.Id)));
    }

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
        *Output = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(InspectLockHeld().c_str()).release();
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to inspect container '%hs'", m_id.c_str());
}

std::string WSLCContainerImpl::InspectLockHeld() const
{
    // Get Docker inspect data
    auto dockerInspect = m_runtime.Docker().InspectContainer(m_id);

    // Convert to WSLC schema
    auto wslcInspect = BuildInspectContainer(dockerInspect);

    // Serialize WSLC schema to JSON
    return wsl::shared::ToJson(wslcInspect);
}

void WSLCContainerImpl::Logs(WSLCLogsFlags Flags, WSLCHandle* Stdout, WSLCHandle* Stderr, LONGLONG Since, LONGLONG Until, ULONGLONG Tail) const
{
    auto lock = m_lock.lock_shared();

    wil::unique_socket socket;
    try
    {
        socket = m_runtime.Docker().ContainerLogs(m_id, Flags, Since, Until, Tail);
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to get logs from '%hs'", m_id.c_str());

    if (WI_IsFlagSet(m_initProcessFlags, WSLCProcessFlagsTty))
    {
        // For tty processes, simply relay the HTTP chunks.
        auto [ttyRead, ttyWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);

        auto handle = std::make_unique<RelayHandle<HTTPChunkBasedReadHandle>>(std::move(socket), std::move(ttyWrite));
        m_runtime.Relay()->AddHandle(std::move(handle));

        *Stdout = common::wslutil::ToCOMOutputHandle(ttyRead.get(), GENERIC_READ | SYNCHRONIZE, WSLCHandleTypePipe);
    }
    else
    {
        // For non-tty process, stdout & stderr are multiplexed.
        auto [stdoutRead, stdoutWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);
        auto [stderrRead, stderrWrite] = common::wslutil::OpenAnonymousPipe(0, true, true);

        auto handle = std::make_unique<DockerIORelayHandle>(
            std::move(socket), std::move(stdoutWrite), std::move(stderrWrite), DockerIORelayHandle::Format::HttpChunked);

        m_runtime.Relay()->AddHandle(std::move(handle));

        *Stdout = common::wslutil::ToCOMOutputHandle(stdoutRead.get(), GENERIC_READ | SYNCHRONIZE, WSLCHandleTypePipe);
        *Stderr = common::wslutil::ToCOMOutputHandle(stderrRead.get(), GENERIC_READ | SYNCHRONIZE, WSLCHandleTypePipe);
    }
}

void WSLCContainerImpl::Stats(LPSTR* Output) const
{
    auto lock = m_lock.lock_shared();

    try
    {
        auto stats = m_runtime.Docker().ContainerStats(m_id);

        // Always inject the authoritative id and name from this instance.
        // The response may omit them or use inconsistent casing.
        stats.id = m_id;
        stats.name = m_name;

        std::string json = wsl::shared::ToJson(stats);
        *Output = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(json.c_str()).release();
    }
    CATCH_AND_THROW_DOCKER_USER_ERROR("Failed to get stats for container '%hs'", m_id.c_str());
}

std::unique_ptr<RelayedProcessIO> WSLCContainerImpl::CreateRelayedProcessIO(wil::shared_socket stream, WSLCProcessFlags flags)
{
    // Create one pipe for each STD handle.
    std::vector<std::unique_ptr<OverlappedIOHandle>> ioHandles;
    std::map<ULONG, TypedHandle> fds;

    // This is required for docker to know when stdin is closed.
    auto closeStdin = [stream]() { LOG_LAST_ERROR_IF(shutdown(stream.get(), SD_SEND) == SOCKET_ERROR); };

    if (WI_IsFlagSet(flags, WSLCProcessFlagsStdin))
    {
        auto [stdinRead, stdinWrite] = common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);
        ioHandles.emplace_back(std::make_unique<RelayHandle<ReadHandle>>(
            HandleWrapper{std::move(stdinRead), std::move(closeStdin)}, HandleWrapper{stream}));

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
        HandleWrapper{stream}, std::move(stdoutWrite), std::move(stderrWrite), common::io::DockerIORelayHandle::Format::Raw));

    m_runtime.Relay()->AddHandles(std::move(ioHandles));

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
            // This is required because the same container can bind the port number for different families or protocols.
            auto existing = allocatedPorts.find(e.ContainerPort);
            if (existing != allocatedPorts.end())
            {
                e.VmMapping.AssignVmPort(existing->second);
            }
            else
            {
                auto allocatedPort =
                    m_runtime.Vm().TryAllocatePort(e.ContainerPort, e.VmMapping.BindAddress.si_family, e.VmMapping.Protocol);

                THROW_HR_WITH_USER_ERROR_IF(
                    HRESULT_FROM_WIN32(WSAEADDRINUSE), wsl::shared::Localization::MessageWslcPortInUse(FormatPortEndpoint(e), m_id), !allocatedPort);

                e.VmMapping.AssignVmPort(allocatedPort);

                allocatedPorts.emplace(e.ContainerPort, allocatedPort);
            }
        }

        try
        {
            m_runtime.Vm().MapPort(e.VmMapping);
        }
        catch (...)
        {
            auto result = wil::ResultFromCaughtException();
            if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) || result == HRESULT_FROM_WIN32(WSAEADDRINUSE))
            {
                THROW_HR_WITH_USER_ERROR(
                    HRESULT_FROM_WIN32(WSAEADDRINUSE), wsl::shared::Localization::MessageWslcPortInUse(FormatPortEndpoint(e), m_id));
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
            if (m_networkMode == "host")
            {
                e.VmMapping.VmPort.reset();
            }
        }
        CATCH_LOG();
    }
}

__requires_exclusive_lock_held(m_lock) void WSLCContainerImpl::ReleaseProcesses()
{
    // Snapshot under the lock, then notify outside it, pinning each control via lock() first
    decltype(m_processes) processes;
    {
        std::lock_guard processesLock{m_processesLock};
        processes = std::exchange(m_processes, {});
    }

    // Notify all processes that the container has exited.
    // The exec callback isn't always sent to execed processes, so do this to avoid 'stuck' processes.
    for (auto& process : processes)
    {
        if (auto control = process.lock())
        {
            control->OnContainerReleased();
        }
    }
}

__requires_exclusive_lock_held(m_lock) void WSLCContainerImpl::ReleaseRuntimeResources()
{
    WSL_LOG("ReleaseRuntimeResources", TraceLoggingValue(m_id.c_str(), "ID"));

    // Release runtime resources (port relays, volume mounts) that were set up at Start().
    UnmapPorts();

    // A VM that already exited (crash / external kill) has dropped every guest mount, so calling
    // UnmountWindowsFolder would only block on the RPC timeout and emit spurious unmount-failed
    // warnings. Mark the mounts inactive without touching the dead VM.
    //
    // The same applies when there is no VM at all: after a graceful idle teardown the VM object is
    // released without the exit event ever being signaled, so VmExited() is false while HasVm() is
    // false too. m_runtime.Vm() would throw on that state, and this runs from ~WSLCContainerImpl.
    if (m_runtime.VmExited() || !m_runtime.HasVm())
    {
        for (auto& volume : m_mountedVolumes)
        {
            volume.Mounted = false;
        }
    }
    else
    {
        UnmountVolumes(m_mountedVolumes, m_runtime.Vm());
    }
}

__requires_exclusive_lock_held(m_lock) unique_com_disconnect WSLCContainerImpl::ReleaseResources()
{
    WSL_LOG("ReleaseResources", TraceLoggingValue(m_id.c_str(), "ID"));

    ReleaseRuntimeResources();

    // Release VM port allocations back to the pool.
    for (auto& e : m_mappedPorts)
    {
        e.VmMapping.VmPort.reset();
    }

    return PrepareDisconnectComWrapper();
}

__requires_exclusive_lock_held(m_lock) unique_com_disconnect WSLCContainerImpl::PrepareDisconnectComWrapper()
{
    if (m_comWrapper)
    {
        // Cache read-only properties in the COM wrapper before disconnecting,
        // so callers can still query state/process after the impl is gone.
        {
            std::lock_guard processesLock{m_processesLock};
            m_comWrapper->CacheState(m_id, m_name, m_state, m_initProcess);
        }
    }

    return unique_com_disconnect{std::exchange(m_comWrapper, nullptr)};
}

__requires_lock_held(m_lock) void WSLCContainerImpl::CommitState(WSLCContainerState State, std::optional<std::int64_t> stateChangedAt) noexcept
{
    // N.B. A deleted container cannot transition back to any other state.
    WI_ASSERT(m_state != WslcContainerStateDeleted);

    WSL_LOG(
        "ContainerStateChange",
        TraceLoggingValue(static_cast<int>(m_state), "PreviousState"),
        TraceLoggingValue(static_cast<int>(State), "NewState"),
        TraceLoggingValue(m_id.c_str(), "ID"));

    m_state = State;
    m_stateGeneration++;
    m_stateChangedAt = stateChangedAt.value_or(static_cast<std::int64_t>(std::time(nullptr)));

    // Keep the VM alive while this container is Running and release the hold once it leaves that
    // state, even when no client holds the wrapper (e.g. a detached `run -d` container). Dropping
    // the hold on the transition out of Running is what lets an otherwise-idle VM be torn down; a
    // Created or Exited container does not pin the VM, since its metadata survives teardown.
    UpdateActivityHoldLockHeld();
}

__requires_lock_held(m_lock) void WSLCContainerImpl::UpdateActivityHoldLockHeld() noexcept
{
    const bool active = (m_state == WslcContainerStateRunning);
    if (active && !m_activityHold)
    {
        m_activityHold = ActivityRef(m_wslcSession.Runtime().IdleStateShared());
    }
    else if (!active && m_activityHold)
    {
        m_activityHold.reset();
    }
}

WSLCContainer::WSLCContainer(WSLCSession& session, std::function<void(const WSLCContainerImpl*)>&& OnDeleted) :
    m_session(session), m_onDeleted(std::move(OnDeleted))
{
}

HRESULT WSLCContainer::Attach(LPCSTR DetachKeys, WSLCHandle* Stdin, WSLCHandle* Stdout, WSLCHandle* Stderr)
try
{
    WSLCExecutionContext context(&m_session);

    RETURN_HR_IF_NULL(E_POINTER, Stdin);
    RETURN_HR_IF_NULL(E_POINTER, Stdout);
    RETURN_HR_IF_NULL(E_POINTER, Stderr);

    *Stdin = {};
    *Stdout = {};
    *Stderr = {};

    auto vmLease = m_session.Runtime().AcquireVmLease();
    return CallImpl(&WSLCContainerImpl::Attach, DetachKeys, Stdin, Stdout, Stderr);
}
CATCH_RETURN();

HRESULT WSLCContainer::GetState(WSLCContainerState* Result)
{
    WSLCExecutionContext context(&m_session);
    RETURN_HR_IF_NULL(E_POINTER, Result);

    *Result = WslcContainerStateInvalid;
    HRESULT hr = CallImpl(&WSLCContainerImpl::GetState, Result);
    if (SUCCEEDED(hr))
    {
        return S_OK;
    }

    // PrepareDisconnectComWrapper() populates the cache before setting m_impl to null,
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
    WSLCExecutionContext context(&m_session);

    RETURN_HR_IF_NULL(E_POINTER, Process);

    *Process = nullptr;

    HRESULT hr = CallImpl(&WSLCContainerImpl::GetInitProcess, Process);
    if (SUCCEEDED(hr))
    {
        return S_OK;
    }

    // PrepareDisconnectComWrapper() populates the cache before setting m_impl to null,
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

HRESULT WSLCContainer::Exec(const WSLCProcessOptions* Options, const WSLCProcessStartOptions* StartOptions, IWSLCProcess** Process)
try
{
    WSLCExecutionContext context(&m_session);

    RETURN_HR_IF_NULL(E_POINTER, Options);
    RETURN_HR_IF_NULL(E_POINTER, Process);
    RETURN_HR_IF_MSG(E_INVALIDARG, WI_IsAnyFlagSet(Options->Flags, ~WSLCProcessFlagsValid), "Invalid flags: 0x%x", Options->Flags);

    *Process = nullptr;

    auto vmLease = m_session.Runtime().AcquireVmLease();
    return CallImpl(&WSLCContainerImpl::Exec, Options, StartOptions, Process);
}
CATCH_RETURN();

HRESULT WSLCContainer::Stop(_In_ WSLCSignal Signal, _In_ LONG TimeoutSeconds)
try
{
    WSLCExecutionContext context(&m_session);

    // Hold a VM lease for the whole operation: --rm containers self-delete during Stop, which
    // disconnects the wrapper and drops activity. Without the lease, the idle worker can fire
    // during the post-stop destroy wait (up to 60s) and tear the VM down mid-call.
    auto vmLease = m_session.Runtime().AcquireVmLease();
    return CallImpl(&WSLCContainerImpl::Stop, Signal, TimeoutSeconds, false);
}
CATCH_RETURN();

HRESULT WSLCContainer::Kill(_In_ WSLCSignal Signal)
try
{
    WSLCExecutionContext context(&m_session);

    // Hold a VM lease for the same reason as Stop(): --rm can self-delete and drop activity.
    auto vmLease = m_session.Runtime().AcquireVmLease();
    return CallImpl(&WSLCContainerImpl::Stop, Signal, {}, true);
}
CATCH_RETURN();

HRESULT WSLCContainer::Start(WSLCContainerStartFlags Flags, const WSLCProcessStartOptions* StartOptions, IWarningCallback* WarningCallback)
try
{
    WSLCExecutionContext context(&m_session, WarningCallback);

    THROW_HR_IF_MSG(E_INVALIDARG, WI_IsAnyFlagSet(Flags, ~WSLCContainerStartFlagsValid), "Invalid flags: 0x%x", Flags);

    auto vmLease = m_session.Runtime().AcquireVmLease();
    return CallImpl(&WSLCContainerImpl::Start, Flags, StartOptions);
}
CATCH_RETURN();

HRESULT WSLCContainer::Inspect(LPSTR* Output)
try
{
    WSLCExecutionContext context(&m_session);

    RETURN_HR_IF_NULL(E_POINTER, Output);

    *Output = nullptr;

    auto vmLease = m_session.Runtime().AcquireVmLease();
    return CallImpl(&WSLCContainerImpl::Inspect, Output);
}
CATCH_RETURN();

HRESULT WSLCContainer::Stats(LPSTR* Output)
try
{
    WSLCExecutionContext context(&m_session);

    RETURN_HR_IF(E_POINTER, Output == nullptr);

    *Output = nullptr;

    auto vmLease = m_session.Runtime().AcquireVmLease();
    return CallImpl(&WSLCContainerImpl::Stats, Output);
}
CATCH_RETURN();

HRESULT WSLCContainer::Delete(WSLCDeleteFlags Flags)
try
{
    WSLCExecutionContext context(&m_session);

    THROW_HR_IF_MSG(E_INVALIDARG, WI_IsAnyFlagSet(Flags, ~WSLCDeleteFlagsValid), "Invalid flags: 0x%x", Flags);

    // Special case for Delete(): If deletion is successful, notify the WSLCSession that the container has been deleted.
    // Hold a VM lease across the whole operation: deleting a container makes it inactive and
    // can trigger an idle teardown. Without the lease the idle worker could take the session
    // lock exclusively and clear m_containers (destroying this container) concurrently, racing
    // the delete and inverting the container->session lock order.
    auto vmLease = m_session.Runtime().AcquireVmLease();
    auto [lock, impl] = LockImpl();

    impl->Delete(Flags);
    m_onDeleted(impl.get());

    return S_OK;
}
CATCH_RETURN();

void WSLCContainer::CacheState(const std::string& id, const std::string& name, WSLCContainerState state, const Microsoft::WRL::ComPtr<IWSLCProcess>& initProcess) noexcept
try
{
    auto cacheLock = m_cacheLock.lock_exclusive();

    // CacheState must only be called once, during PrepareDisconnectComWrapper().
    WI_ASSERT(!m_cachedState.has_value());

    m_cachedId = id;
    m_cachedName = name;
    m_cachedState = state;
    m_cachedInitProcess = initProcess;
}
CATCH_LOG();

HRESULT WSLCContainer::Export(WSLCHandle TarHandle)
try
{
    WSLCExecutionContext context(&m_session);

    auto vmLease = m_session.Runtime().AcquireVmLease();
    return CallImpl(&WSLCContainerImpl::Export, TarHandle);
}
CATCH_RETURN();

HRESULT WSLCContainer::UploadArchive(WSLCHandle TarHandle, LPCSTR DestPath, ULONGLONG ContentSize)
try
{
    WSLCExecutionContext context(&m_session);

    RETURN_HR_IF(E_POINTER, DestPath == nullptr);
    RETURN_HR_IF(E_INVALIDARG, DestPath[0] == '\0');

    auto vmLease = m_session.Runtime().AcquireVmLease();
    return CallImpl(&WSLCContainerImpl::UploadArchive, TarHandle, DestPath, ContentSize);
}
CATCH_RETURN();

HRESULT WSLCContainer::DownloadArchive(LPCSTR SrcPath, WSLCHandle OutHandle)
try
{
    WSLCExecutionContext context(&m_session);

    RETURN_HR_IF(E_POINTER, SrcPath == nullptr);
    RETURN_HR_IF(E_INVALIDARG, SrcPath[0] == '\0');

    auto vmLease = m_session.Runtime().AcquireVmLease();
    return CallImpl(&WSLCContainerImpl::DownloadArchive, SrcPath, OutHandle);
}
CATCH_RETURN();

HRESULT WSLCContainer::Logs(WSLCLogsFlags Flags, WSLCHandle* Stdout, WSLCHandle* Stderr, LONGLONG Since, LONGLONG Until, ULONGLONG Tail)
try
{
    WSLCExecutionContext context(&m_session);
    RETURN_HR_IF(E_POINTER, Stdout == nullptr || Stderr == nullptr);

    THROW_HR_IF_MSG(E_INVALIDARG, WI_IsAnyFlagSet(Flags, ~WSLCLogsFlagsValid), "Invalid flags: 0x%x", Flags);

    *Stdout = {};
    *Stderr = {};

    auto vmLease = m_session.Runtime().AcquireVmLease();
    return CallImpl(&WSLCContainerImpl::Logs, Flags, Stdout, Stderr, Since, Until, Tail);
}
CATCH_RETURN();

HRESULT WSLCContainer::GetId(WSLCContainerId Id)
try
{
    WSLCExecutionContext context(&m_session);

    RETURN_HR_IF_NULL(E_POINTER, Id);

    const auto hr = wil::ResultFromException([&] {
        auto [lock, impl] = LockImpl();
        WI_VERIFY(strcpy_s(Id, std::size<char>(WSLCContainerId{}), impl->ID().c_str()) == 0);
    });

    RETURN_HR_IF(hr, hr != RPC_E_DISCONNECTED);

    // PrepareDisconnectComWrapper() populates the cache before setting m_impl to null,
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
    WSLCExecutionContext context(&m_session);

    RETURN_HR_IF_NULL(E_POINTER, Name);
    *Name = nullptr;

    const auto hr = wil::ResultFromException([&] {
        auto [lock, impl] = LockImpl();
        *Name = wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(impl->Name().c_str()).release();
    });

    RETURN_HR_IF(hr, hr != RPC_E_DISCONNECTED);

    // PrepareDisconnectComWrapper() populates the cache before setting m_impl to null,
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

void WSLCContainerImpl::ConnectToNetwork(const WSLCNetworkConnectionOptions* Options)
{
    THROW_HR_IF(E_POINTER, Options == nullptr);

    THROW_HR_WITH_USER_ERROR_IF(
        E_INVALIDARG, Localization::MessageWslcNetworkNameRequired(), !Options->NetworkName || strlen(Options->NetworkName) == 0);

    auto endpointConfig = ResolveEndpointConfig(Options->Settings, Options->SettingsCount, Options->NetworkName);

    auto lock = m_lock.lock_shared();

    THROW_HR_WITH_USER_ERROR_IF(
        E_INVALIDARG, Localization::MessageWslcNetworkModeNoAdditionalNetworks(m_networkMode), !NetworkModeAllocatesVmPorts(m_networkMode));

    common::docker_schema::ContainerNetworkRequest request{};
    request.Container = m_id;
    request.EndpointConfig = std::move(endpointConfig);

    try
    {
        m_runtime.Docker().ConnectContainerToNetwork(Options->NetworkName, request);
    }
    catch (const DockerHTTPException& e)
    {
        THROW_HR_WITH_USER_ERROR_IF(
            WSLC_E_NETWORK_NOT_FOUND, Localization::MessageWslcNetworkNotFound(Options->NetworkName), e.StatusCode() == 404);
        THROW_DOCKER_USER_ERROR_MSG(e, "Failed to connect container '%hs' to network '%hs'", m_id.c_str(), Options->NetworkName);
    }

    WSL_LOG(
        "ContainerConnectedToNetwork",
        TraceLoggingValue(m_id.c_str(), "ContainerId"),
        TraceLoggingValue(Options->NetworkName, "NetworkName"));
}

void WSLCContainerImpl::DisconnectFromNetwork(LPCSTR NetworkName)
{
    THROW_HR_IF(E_POINTER, NetworkName == nullptr);
    THROW_HR_WITH_USER_ERROR_IF(E_INVALIDARG, Localization::MessageWslcNetworkNameRequired(), strlen(NetworkName) == 0);

    auto lock = m_lock.lock_shared();

    THROW_HR_WITH_USER_ERROR_IF(
        E_INVALIDARG, Localization::MessageWslcNetworkModeNoAdditionalNetworks(m_networkMode), !NetworkModeAllocatesVmPorts(m_networkMode));

    common::docker_schema::ContainerNetworkRequest request{};
    request.Container = m_id;

    try
    {
        m_runtime.Docker().DisconnectContainerFromNetwork(NetworkName, request);
    }
    catch (const DockerHTTPException& e)
    {
        THROW_HR_WITH_USER_ERROR_IF(WSLC_E_NETWORK_NOT_FOUND, Localization::MessageWslcNetworkNotFound(NetworkName), e.StatusCode() == 404);
        THROW_DOCKER_USER_ERROR_MSG(e, "Failed to disconnect container '%hs' from network '%hs'", m_id.c_str(), NetworkName);
    }

    WSL_LOG(
        "ContainerDisconnectedFromNetwork",
        TraceLoggingValue(m_id.c_str(), "ContainerId"),
        TraceLoggingValue(NetworkName, "NetworkName"));
}

HRESULT WSLCContainer::GetLabels(WSLCLabelInformation** Labels, ULONG* Count)
try
{
    WSLCExecutionContext context(&m_session);

    RETURN_HR_IF(E_POINTER, Labels == nullptr || Count == nullptr);

    *Count = 0;
    *Labels = nullptr;
    return CallImpl(&WSLCContainerImpl::GetLabels, Labels, Count);
}
CATCH_RETURN();

HRESULT WSLCContainer::ConnectToNetwork(const WSLCNetworkConnectionOptions* Options)
try
{
    COMServiceExecutionContext context;

    auto vmLease = m_session.Runtime().AcquireVmLease();
    return CallImpl(&WSLCContainerImpl::ConnectToNetwork, Options);
}
CATCH_RETURN();

HRESULT WSLCContainer::DisconnectFromNetwork(LPCSTR NetworkName)
try
{
    COMServiceExecutionContext context;

    auto vmLease = m_session.Runtime().AcquireVmLease();
    return CallImpl(&WSLCContainerImpl::DisconnectFromNetwork, NetworkName);
}
CATCH_RETURN();

HRESULT WSLCContainer::InterfaceSupportsErrorInfo(REFIID riid)
{
    return riid == __uuidof(IWSLCContainer) || riid == __uuidof(IWSLCCompatContainer) ? S_OK : S_FALSE;
}

HRESULT WSLCContainer::Start(WSLCContainerStartFlags Flags)
{
    return Start(Flags, nullptr, nullptr);
}

HRESULT WSLCContainer::GetInitProcess(IWSLCCompatProcess** Process)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Process);
    *Process = nullptr;

    Microsoft::WRL::ComPtr<IWSLCProcess> process;
    RETURN_IF_FAILED(GetInitProcess(&process));
    RETURN_HR_IF_NULL(E_UNEXPECTED, process);

    return process.CopyTo(Process);
}
CATCH_RETURN();

HRESULT WSLCContainer::Exec(const WSLCCompatProcessOptions* Options, IWSLCCompatProcess** Process)
try
{
    RETURN_HR_IF_NULL(E_POINTER, Options);
    RETURN_HR_IF_NULL(E_POINTER, Process);
    *Process = nullptr;

    const auto options = apicompat::Convert(*Options);

    Microsoft::WRL::ComPtr<IWSLCProcess> process;
    RETURN_IF_FAILED(Exec(&options, nullptr, &process));
    RETURN_HR_IF_NULL(E_UNEXPECTED, process);

    return process.CopyTo(Process);
}
CATCH_RETURN();
