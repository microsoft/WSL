/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreConfig.h

Abstract:

    This file contains the WSL Core VM configuration helper class declaration.

--*/

#pragma once

#define T_ENUM(c, n) TraceLoggingValue(wsl::core::ToString((c).n), #n)
#define T_PRESENT(c, n) TraceLoggingValue((c).n == ConfigKeyPresence::Present, #n)
#define T_SET(c, n) TraceLoggingValue(!(c).n.empty(), #n "Set")
#define T_STRING(c, n) TraceLoggingValue((c).n.c_str(), #n)
#define T_VALUE(c, n) TraceLoggingValue((c).n, #n)

#define CONFIG_TELEMETRY(c) \
    T_VALUE(c, BestEffortDnsParsing), T_VALUE(c, DhcpTimeout), T_VALUE(c, EnableAutoProxy), T_VALUE(c, EnableDebugConsole), \
        T_VALUE(c, EnableDebugShell), T_VALUE(c, EnableDhcp), T_VALUE(c, EnableDnsProxy), T_VALUE(c, EnableDnsTunneling), \
        T_VALUE(c, EnableGpuSupport), T_VALUE(c, EnableGuiApps), T_VALUE(c, EnableHardwarePerformanceCounters), \
        T_VALUE(c, EnableHostAddressLoopback), T_VALUE(c, EnableHostFileSystemAccess), T_VALUE(c, EnableIpv6), \
        T_VALUE(c, EnableLocalhostRelay), T_VALUE(c, EnableNestedVirtualization), T_VALUE(c, EnableSafeMode), \
        T_VALUE(c, EnableSparseVhd), T_VALUE(c, EnableVirtio), T_VALUE(c, EnableVirtio9p), T_VALUE(c, EnableVirtioFs), \
        T_ENUM(c, FirewallConfigPresence), T_VALUE(c, KernelBootTimeout), T_SET(c, KernelCommandLine), \
        T_VALUE(c, KernelDebugPort), T_SET(c, KernelModulesPath), T_STRING(c, KernelModulesList), T_SET(c, KernelPath), \
        T_VALUE(c, LoadDefaultKernelModules), T_PRESENT(c, LoadKernelModulesPresence), T_VALUE(c, MaximumMemorySizeBytes), \
        T_VALUE(c, MaximumProcessorCount), T_ENUM(c, MemoryReclaim), T_VALUE(c, MemorySizeBytes), T_VALUE(c, MountDeviceTimeout), \
        T_ENUM(c, NetworkingMode), T_VALUE(c, ProcessorCount), T_SET(c, SwapFilePath), T_VALUE(c, SwapSizeBytes), \
        T_SET(c, SystemDistroPath), T_VALUE(c, VhdSizeBytes), T_VALUE(c, VmIdleTimeout), T_SET(c, VmSwitch)

namespace wsl::core {
constexpr auto ToString(ConfigKeyPresence key)
{
    switch (key)
    {
    case ConfigKeyPresence::Absent:
        return "Absent";
    case ConfigKeyPresence::Present:
        return "Present";
    default:
        return "Invalid";
    }
}

enum class MemoryReclaimMode
{
    Disabled,
    Gradual,
    DropCache
};

// Ensure the WslCoreConfig versions of the enum match the version that's used in mini init.
static_assert(static_cast<ULONG>(MemoryReclaimMode::Disabled) == LxMiniInitMemoryReclaimModeDisabled);
static_assert(static_cast<ULONG>(MemoryReclaimMode::Gradual) == LxMiniInitMemoryReclaimModeGradual);
static_assert(static_cast<ULONG>(MemoryReclaimMode::DropCache) == LxMiniInitMemoryReclaimModeDropCache);

constexpr auto ToString(MemoryReclaimMode mode)
{
    switch (mode)
    {
    case MemoryReclaimMode::Disabled:
        return "Disabled";
    case MemoryReclaimMode::Gradual:
        return "Gradual";
    case MemoryReclaimMode::DropCache:
        return "DropCache";
    default:
        return "Invalid";
    }
}

const std::map<std::string, MemoryReclaimMode, shared::string::CaseInsensitiveCompare> MemoryReclaimModes = {
    {ToString(MemoryReclaimMode::Gradual), MemoryReclaimMode::Gradual},
    {ToString(MemoryReclaimMode::DropCache), MemoryReclaimMode::DropCache},
    {ToString(MemoryReclaimMode::Disabled), MemoryReclaimMode::Disabled}};

// N.B. These enum values are also used in InTune ADMX templates, if entries are added or removed ensure that existing
//      values are not changed.
enum NetworkingMode
{
    None = 0,
    Nat = 1,
    Bridged = 2,
    Mirrored = 3,
    VirtioProxy = 4
};

// Ensure the WslCoreConfig versions of the enum match the version that's used in mini init.
static_assert(static_cast<ULONG>(NetworkingMode::None) == LxMiniInitNetworkingModeNone);
static_assert(static_cast<ULONG>(NetworkingMode::Nat) == LxMiniInitNetworkingModeNat);
static_assert(static_cast<ULONG>(NetworkingMode::Bridged) == LxMiniInitNetworkingModeBridged);
static_assert(static_cast<ULONG>(NetworkingMode::Mirrored) == LxMiniInitNetworkingModeMirrored);
static_assert(static_cast<ULONG>(NetworkingMode::VirtioProxy) == LxMiniInitNetworkingModeVirtioProxy);

constexpr auto ToString(NetworkingMode config) noexcept
{
    switch (config)
    {
    case NetworkingMode::None:
        return "None";
    case NetworkingMode::Nat:
        return "Nat";
    case NetworkingMode::Bridged:
        return "Bridged";
    case NetworkingMode::Mirrored:
        return "Mirrored";
    case NetworkingMode::VirtioProxy:
        return "VirtioProxy";
    default:
        return "Invalid";
    }
}

const std::map<std::string, wsl::core::NetworkingMode, shared::string::CaseInsensitiveCompare> NetworkingModes{
    {ToString(NetworkingMode::None), NetworkingMode::None},
    {ToString(NetworkingMode::Nat), NetworkingMode::Nat},
    {ToString(NetworkingMode::Bridged), NetworkingMode::Bridged},
    {ToString(NetworkingMode::Mirrored), NetworkingMode::Mirrored},
    {ToString(NetworkingMode::VirtioProxy), NetworkingMode::VirtioProxy}};

enum class FirewallAction
{
    Invalid,
    Allow,
    Block
};

constexpr auto ToString(const FirewallAction Action) noexcept
{
    switch (Action)
    {
    case FirewallAction::Allow:
        return "Allow";
    case FirewallAction::Block:
        return "Block";
    default:
        return "Invalid";
    }
}

enum class FirewallRuleOperation
{
    Invalid,
    Add,
    Delete
};

struct FirewallRuleConfiguration
{
    // These values are shared_bstr because we make temporary copies (for example in ConfigureHyperVFirewall)
    wil::shared_bstr RuleId;
    wil::shared_bstr RuleName;
    wil::shared_bstr Protocol;
    std::vector<wil::shared_bstr> LocalPorts;
    std::vector<wil::shared_bstr> LocalAddresses;
    std::vector<wil::shared_bstr> RemoteAddresses;
    FirewallRuleOperation RuleOperation;
    // NOTE these are only applicable for HOST firewall rules
    wil::shared_bstr LocalService;
    wil::shared_bstr LocalApplication;

    FirewallRuleConfiguration& operator=(const FirewallRuleConfiguration&) = default;
    FirewallRuleConfiguration& operator=(FirewallRuleConfiguration&&) = default;
    FirewallRuleConfiguration(FirewallRuleConfiguration&&) = default;
    FirewallRuleConfiguration(const FirewallRuleConfiguration&) = default;

    FirewallRuleConfiguration(
        _In_ LPCWSTR RuleIdParam,
        _In_opt_ LPCWSTR RuleNameParam = nullptr,
        _In_opt_ LPCWSTR ProtocolParam = nullptr,
        _In_ DWORD LocalPortsCountParam = 0,
        _In_reads_opt_(LocalPortsCountParam) LPCWSTR* LocalPortsParam = nullptr,
        _In_ DWORD LocalAddressesCountParam = 0,
        _In_reads_opt_(LocalAddressesCountParam) LPCWSTR* LocalAddressesParam = nullptr,
        _In_ DWORD RemoteAddressesCountParam = 0,
        _In_reads_opt_(RemoteAddressesCountParam) LPCWSTR* RemoteAddressesParam = nullptr,
        _In_opt_ LPCWSTR LocalServiceParam = nullptr,
        _In_opt_ LPCWSTR LocalApplicationParam = nullptr,
        _In_ FirewallRuleOperation RuleOperationParam = FirewallRuleOperation::Add)
    {
        RuleId = wil::make_bstr(RuleIdParam);
        if (RuleNameParam)
        {
            RuleName = wil::make_bstr(RuleNameParam);
        }
        if (ProtocolParam)
        {
            Protocol = wil::make_bstr(ProtocolParam);
        }
        for (ULONG i = 0; i < LocalPortsCountParam; ++i)
        {
            LocalPorts.emplace_back(wil::make_bstr(LocalPortsParam[i]));
        }
        for (ULONG i = 0; i < LocalAddressesCountParam; ++i)
        {
            LocalAddresses.emplace_back(wil::make_bstr(LocalAddressesParam[i]));
        }
        for (ULONG i = 0; i < RemoteAddressesCountParam; ++i)
        {
            RemoteAddresses.emplace_back(wil::make_bstr(RemoteAddressesParam[i]));
        }
        if (LocalServiceParam)
        {
            LocalService = wil::make_bstr(LocalServiceParam);
        }
        if (LocalApplicationParam)
        {
            LocalApplication = wil::make_bstr(LocalApplicationParam);
        }
        RuleOperation = RuleOperationParam;
    }
};

struct FirewallConfiguration
{
    std::optional<GUID> VmCreatorId{};
    std::vector<FirewallRuleConfiguration> Rules{};
    FirewallAction DefaultLoopbackPolicy{FirewallAction::Invalid};

    bool Enabled() const noexcept;

    void reset() noexcept;

    void Enable() noexcept;
};

namespace ConfigSetting {
    static constexpr auto Kernel = "wsl2.kernel";
    static constexpr auto KernelCommandLine = "wsl2.kernelCommandLine";
    static constexpr auto KernelModules = "wsl2.kernelModules";
    static constexpr auto Memory = "wsl2.memory";
    static constexpr auto Processors = "wsl2.processors";
    static constexpr auto DebugConsole = "wsl2.debugConsole";
    static constexpr auto EarlyBootLogging = "wsl2.earlyBootLogging";
    static constexpr auto Swap = "wsl2.swap";
    static constexpr auto SwapFile = "wsl2.swapFile";
    static constexpr auto LocalhostForwarding = "wsl2.localhostForwarding";
    static constexpr auto NestedVirtualization = "wsl2.nestedVirtualization";
    static constexpr auto Virtio9p = "wsl2.virtio9p";
    static constexpr auto Virtiofs = "wsl2.virtiofs";
    static constexpr auto KernelDebugPort = "wsl2.kernelDebugPort";
    static constexpr auto GpuSupport = "wsl2.gpuSupport";
    static constexpr auto GuiApplications = "wsl2.guiApplications";
    static constexpr auto SystemDistro = "wsl2.systemDistro";
    static constexpr auto Telemetry = "wsl2.telemetry";
    static constexpr auto VmIdleTimeout = "wsl2.vmIdleTimeout";
    static constexpr auto DebugConsoleLogFile = "wsl2.debugConsoleLogFile";
    static constexpr auto KernelBootTimeout = "wsl2.kernelBootTimeout";
    static constexpr auto DistributionStartTimeout = "wsl2.distributionStartTimeout";
    static constexpr auto Virtio = "wsl2.virtio";
    static constexpr auto HostFileSystemAccess = "wsl2.hostFileSystemAccess";
    static constexpr auto MountDeviceTimeout = "wsl2.mountDeviceTimeout";
    static constexpr auto HardwarePerformanceCounters = "wsl2.hardwarePerformanceCounters";
    static constexpr auto NetworkingMode = "wsl2.networkingMode";
    static constexpr auto VmSwitch = "wsl2.vmSwitch";
    static constexpr auto MacAddress = "wsl2.macAddress";
    static constexpr auto Dhcp = "wsl2.dhcp";
    static constexpr auto DhcpTimeout = "wsl2.dhcpTimeout";
    static constexpr auto Ipv6 = "wsl2.ipv6";
    static constexpr auto DnsProxy = "wsl2.dnsProxy";
    static constexpr auto SafeMode = "wsl2.safeMode";
    static constexpr auto DefaultVhdSize = "wsl2.defaultVhdSize";
    static constexpr auto CrashDumpFolder = "wsl2.crashDumpFolder";
    static constexpr auto MaxCrashDumpCount = "wsl2.maxCrashDumpCount";
    static constexpr auto DistributionInstallPath = "general.distributionInstallPath";
    static constexpr auto InstanceIdleTimeout = "general.instanceIdleTimeout";
    static constexpr auto DnsTunneling = "wsl2.dnsTunneling";
    static constexpr auto Firewall = "wsl2.firewall";
    static constexpr auto AutoProxy = "wsl2.autoProxy";
    static constexpr auto LoadKernelModules = "wsl2.loadKernelModules";
    static constexpr auto LoadDefaultKernelModules = "wsl2.loadDefaultKernelModules";

    namespace Experimental {
        static constexpr auto NetworkingMode = "experimental.networkingMode";
        static constexpr auto AutoMemoryReclaim = "experimental.autoMemoryReclaim";
        static constexpr auto SparseVhd = "experimental.sparseVhd";
        static constexpr auto DnsTunneling = "experimental.dnsTunneling";
        static constexpr auto BestEffortDnsParsing = "experimental.bestEffortDnsParsing";
        static constexpr auto DnsTunnelingIpAddress = "experimental.dnsTunnelingIpAddress";
        static constexpr auto Firewall = "experimental.firewall";
        static constexpr auto AutoProxy = "experimental.autoProxy";
        static constexpr auto InitialAutoProxyTimeout = "experimental.initialAutoProxyTimeout";
        static constexpr auto IgnoredPorts = "experimental.ignoredPorts";
        static constexpr auto HostAddressLoopback = "experimental.hostAddressLoopback";
        static constexpr auto SetVersionDebug = "experimental.setVersionDebug";

    } // namespace Experimental
} // namespace ConfigSetting

struct Config
{
    Config() = delete;
    Config(_In_opt_ LPCWSTR Path = nullptr, _In_opt_ HANDLE UserToken = nullptr);
    ~Config() = default;

    void Initialize(_In_opt_ HANDLE UserToken = nullptr);
    void ParseConfigFile(_In_opt_ LPCWSTR ConfigFilePath, _In_opt_ HANDLE UserToken);
    void SaveNetworkingSettings(_In_opt_ HANDLE UserToken) const;
    static unsigned long WriteConfigFile(_In_ LPCWSTR ConfigFilePath, _In_ ConfigKey KeyToWrite, _In_ bool RemoveKey = false);

    std::filesystem::path KernelPath;
    std::wstring KernelCommandLine;
    std::wstring KernelModulesList;
    std::filesystem::path KernelModulesPath;
    UINT64 MemorySizeBytes = 0;
    UINT64 MaximumMemorySizeBytes = 0;
    int ProcessorCount = 0;
    int MaximumProcessorCount = 0;
    bool EnableDebugConsole = false;
    bool EnableEarlyBootLogging = false;
    UINT64 SwapSizeBytes = UINT64_MAX;
    std::filesystem::path SwapFilePath;
    bool EnableLocalhostRelay = true;
    ConfigKeyPresence LocalhostRelayConfigPresence = ConfigKeyPresence::Absent;
    ConfigKeyPresence LoadKernelModulesPresence = ConfigKeyPresence::Absent;
    bool LoadDefaultKernelModules = true;
    bool EnableNestedVirtualization = !shared::Arm64 && windows::common::helpers::IsWindows11OrAbove();
    bool EnableVirtio9p = false;
    bool EnableVirtio = !shared::Arm64 || windows::common::helpers::IsWindows11OrAbove();
    bool EnableVirtioFs = false;
    int KernelDebugPort = 0;
    bool EnableGpuSupport = true;
    bool EnableGuiApps = true;
    std::filesystem::path SystemDistroPath;
    bool EnableTelemetry = shared::OfficialBuild;
    int VmIdleTimeout = (60 * 1000);
    int InstanceIdleTimeout = (15 * 1000);
    std::filesystem::path DebugConsoleLogFile;
    std::wstring VmSwitch;
    int KernelBootTimeout = (30 * 1000);
    int DistributionStartTimeout = (60 * 1000);
    int MountDeviceTimeout = (5 * 1000);
    bool EnableHostFileSystemAccess = true;
    bool EnableDhcp = true;
    bool EnableIpv6 = false;
    int DhcpTimeout = (5 * 1000);
    NetworkingMode NetworkingMode = NetworkingMode::Nat;
    ConfigKeyPresence NetworkingModePresence = ConfigKeyPresence::Absent;
    bool EnableDnsProxy = true;
    bool EnableSafeMode = false;
    bool EnableDnsTunneling = true;
    std::filesystem::path DefaultDistributionLocation;
    ConfigKeyPresence DnsTunnelingConfigPresence = ConfigKeyPresence::Absent;
    // Only applicable when DNS tunneling is enabled
    //
    // In a DNS request from Linux there might be DNS records that Windows DNS client does not know how to parse.
    // By default in this case Windows will fail the request. When the flag is enabled, Windows will extract the
    // question from the DNS request and attempt to resolve it, ignoring the unknown records
    bool BestEffortDnsParsing = false;
    // Only applicable when DNS tunneling is enabled
    // IP address that will be used by the DNS listener/proxy used for DNS tunneling. Some scenarios (such as native Docker)
    // require Linux nameserver to be an IP that is not in the range 127.0.0.0/8. This config is intended for those scenarios.
    std::optional<uint32_t> DnsTunnelingIpAddress;
    bool EnableHardwarePerformanceCounters = !shared::Arm64;
    bool EnableAutoProxy = true;
    int InitialAutoProxyTimeout = 1000;
    MemoryReclaimMode MemoryReclaim = MemoryReclaimMode::DropCache;
    bool EnableSparseVhd = false;
    UINT64 VhdSizeBytes = 0x10000000000; // 1TB

    wsl::shared::string::MacAddress MacAddress;
    std::wstring NatIpAddress;
    std::wstring NatGateway;
    std::wstring NatNetwork;
    bool EnableDebugShell = true;
    FirewallConfiguration FirewallConfig;
    ConfigKeyPresence FirewallConfigPresence = ConfigKeyPresence::Absent;
    std::set<uint16_t> IgnoredPorts;
    bool EnableHostAddressLoopback = false;
    std::filesystem::path CrashDumpFolder;
    int MaxCrashDumpCount = 10;

    // Temporary config value to help root cause the truncated archive errors in SetVersion()
    bool SetVersionDebug = false;

    GUID NatNetworkId() const noexcept;
    LPCWSTR NatNetworkName() const noexcept;
};
} // namespace wsl::core
