/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreConfig.cpp

Abstract:

    This file contains the WSL Core VM configuration helper class definition.

--*/

#include "precomp.h"
#include "WslCoreConfig.h"
#include "Localization.h"
#include "WslCoreFirewallSupport.h"
#include "WslCoreNetworkingSupport.h"

constexpr auto c_natGatewayAddress = L"NatGatewayIpAddress";
constexpr auto c_natNetwork = L"NatNetwork";
constexpr auto c_natIpAddress = L"NatIpAddress";

wsl::core::Config::Config(_In_opt_ LPCWSTR Path, _In_opt_ HANDLE UserToken)
{
    ParseConfigFile(Path, UserToken);
    Initialize(UserToken);
}

void wsl::core::Config::ParseConfigFile(_In_opt_ LPCWSTR ConfigFilePath, _In_opt_ HANDLE UserToken)
{
    windows::common::ExecutionContext context(windows::common::ParseConfig);

    auto parseIgnoredPorts = [&](const char* name, const char* value, const wchar_t* fileName, unsigned long fileLine) {
        const auto ignoredPortsVector = wsl::shared::string::Split(std::string{value}, ',');
        for (const auto& portString : ignoredPortsVector)
        {
            int number = 0;
            if (FAILED(wil::ResultFromException([&]() { number = std::stoi(portString); })) || (number <= 0 || number > USHRT_MAX))
            {
                EMIT_USER_WARNING(shared::Localization::MessageConfigInvalidInteger(value, name, fileName, fileLine));
            }
            else
            {
                IgnoredPorts.insert(static_cast<uint16_t>(number));
            }
        }
    };

    auto parseDnsTunnelingIp = [&](const char* name, const char* value, const wchar_t* fileName, unsigned long fileLine) {
        // If the IP is invalid, DNS tunneling is disabled.
        in_addr address{};

        if (inet_pton(AF_INET, value, &address) != 1)
        {
            EMIT_USER_WARNING(shared::Localization::MessageConfigInvalidIp(value, name, fileName, fileLine));
            EnableDnsTunneling = false;
        }
        else
        {
            DnsTunnelingIpAddress = address.S_un.S_addr;
        }
    };

    ConfigKeyPresence earlyBootLoggingPresent{};
    ConfigKeyPresence macAddressPresent{};
    bool enableFirewall = true;
    std::wstring userKernelModules;

    std::vector<ConfigKey> keys{
        ConfigKey(ConfigSetting::Kernel, KernelPath),
        ConfigKey(ConfigSetting::KernelCommandLine, KernelCommandLine),
        ConfigKey(ConfigSetting::KernelModules, KernelModulesPath),
        ConfigKey(ConfigSetting::Memory, MemoryString(MemorySizeBytes)),
        ConfigKey(ConfigSetting::Processors, ProcessorCount),
        ConfigKey(ConfigSetting::DebugConsole, EnableDebugConsole),
        ConfigKey(ConfigSetting::EarlyBootLogging, EnableEarlyBootLogging, &earlyBootLoggingPresent),
        ConfigKey(ConfigSetting::Swap, MemoryString(SwapSizeBytes)),
        ConfigKey(ConfigSetting::SwapFile, SwapFilePath),
        ConfigKey(ConfigSetting::LocalhostForwarding, EnableLocalhostRelay, &LocalhostRelayConfigPresence),
        ConfigKey(ConfigSetting::NestedVirtualization, EnableNestedVirtualization),
        ConfigKey(ConfigSetting::Virtio9p, EnableVirtio9p),
        ConfigKey(ConfigSetting::Virtiofs, EnableVirtioFs),
        ConfigKey(ConfigSetting::KernelDebugPort, KernelDebugPort),
        ConfigKey(ConfigSetting::GpuSupport, EnableGpuSupport),
        ConfigKey(ConfigSetting::GuiApplications, EnableGuiApps),
        ConfigKey(ConfigSetting::SystemDistro, SystemDistroPath),
        ConfigKey(ConfigSetting::Telemetry, EnableTelemetry),
        ConfigKey(ConfigSetting::VmIdleTimeout, VmIdleTimeout),
        ConfigKey(ConfigSetting::DebugConsoleLogFile, DebugConsoleLogFile),
        ConfigKey(ConfigSetting::KernelBootTimeout, KernelBootTimeout),
        ConfigKey(ConfigSetting::DistributionStartTimeout, DistributionStartTimeout),
        ConfigKey(ConfigSetting::Virtio, EnableVirtio),
        ConfigKey(ConfigSetting::HostFileSystemAccess, EnableHostFileSystemAccess),
        ConfigKey(ConfigSetting::MountDeviceTimeout, MountDeviceTimeout),
        ConfigKey(ConfigSetting::HardwarePerformanceCounters, EnableHardwarePerformanceCounters),
        ConfigKey(ConfigSetting::VmSwitch, VmSwitch),
        ConfigKey(ConfigSetting::MacAddress, MacAddress, &macAddressPresent),
        ConfigKey(ConfigSetting::Dhcp, EnableDhcp),
        ConfigKey(ConfigSetting::DhcpTimeout, DhcpTimeout),
        ConfigKey(ConfigSetting::Ipv6, EnableIpv6),
        ConfigKey(ConfigSetting::DnsProxy, EnableDnsProxy),
        ConfigKey(ConfigSetting::SafeMode, EnableSafeMode),
        ConfigKey(ConfigSetting::DefaultVhdSize, MemoryString(VhdSizeBytes)),
        ConfigKey(ConfigSetting::CrashDumpFolder, CrashDumpFolder),
        ConfigKey(ConfigSetting::MaxCrashDumpCount, MaxCrashDumpCount),
        ConfigKey(ConfigSetting::DistributionInstallPath, DefaultDistributionLocation),
        ConfigKey(ConfigSetting::InstanceIdleTimeout, InstanceIdleTimeout),
        ConfigKey(ConfigSetting::LoadDefaultKernelModules, LoadDefaultKernelModules, &LoadKernelModulesPresence),
        ConfigKey(ConfigSetting::LoadKernelModules, userKernelModules, &LoadKernelModulesPresence),

        // Features that were previously experimental (the old header is maintained for compatibility).
        ConfigKey({ConfigSetting::NetworkingMode, ConfigSetting::Experimental::NetworkingMode}, wsl::core::NetworkingModes, NetworkingMode, &NetworkingModePresence),
        ConfigKey({ConfigSetting::DnsTunneling, ConfigSetting::Experimental::DnsTunneling}, EnableDnsTunneling, &DnsTunnelingConfigPresence),
        ConfigKey({ConfigSetting::Firewall, ConfigSetting::Experimental::Firewall}, enableFirewall, &FirewallConfigPresence),
        ConfigKey({ConfigSetting::AutoProxy, ConfigSetting::Experimental::AutoProxy}, EnableAutoProxy),

        // Experimental features.
        ConfigKey(ConfigSetting::Experimental::AutoMemoryReclaim, wsl::core::MemoryReclaimModes, MemoryReclaim),
        ConfigKey(ConfigSetting::Experimental::SparseVhd, EnableSparseVhd),
        ConfigKey(ConfigSetting::Experimental::BestEffortDnsParsing, BestEffortDnsParsing),
        ConfigKey(ConfigSetting::Experimental::DnsTunnelingIpAddress, std::move(parseDnsTunnelingIp)),
        ConfigKey(ConfigSetting::Experimental::InitialAutoProxyTimeout, InitialAutoProxyTimeout),
        ConfigKey(ConfigSetting::Experimental::IgnoredPorts, std::move(parseIgnoredPorts)),
        ConfigKey(ConfigSetting::Experimental::HostAddressLoopback, EnableHostAddressLoopback),
        ConfigKey(ConfigSetting::Experimental::SetVersionDebug, SetVersionDebug)};

    wil::unique_file ConfigFile;
    if (ConfigFilePath != nullptr)
    {
        ConfigFile.reset(_wfopen(ConfigFilePath, L"rt,ccs=UTF-8"));
        if (!ConfigFile)
        {
            const auto error = _doserrno;
            LOG_WIN32_MSG(error, "opening config file failed");
            if (error != ERROR_FILE_NOT_FOUND)
            {
                EMIT_USER_WARNING(wsl::shared::Localization::MessageFailedToOpenConfigFile(
                    ConfigFilePath, wsl::windows::common::wslutil::GetErrorString(HRESULT_FROM_WIN32(error))));
            }
        }
    }

    // Parse the configuration keys.
    WI_VERIFY(::ParseConfigFile(keys, ConfigFile.get(), (CFG_SKIP_INVALID_LINES | CFG_SKIP_UNKNOWN_VALUES), ConfigFilePath) == 0);

    // Hyper-V firewall must always be configured for Mirrored Mode.
    // For NAT mode, we use the experimental config to determine if Hyper-V firewall should be enabled
    if ((NetworkingMode::Mirrored == NetworkingMode) || enableFirewall)
    {
        FirewallConfig.Enable();
    }

    if (EnableDnsTunneling && !DnsTunnelingIpAddress.has_value())
    {
        in_addr address{};
        WI_VERIFY(inet_pton(AF_INET, LX_INIT_DNS_TUNNELING_IP_ADDRESS, &address) == 1);

        DnsTunnelingIpAddress = address.S_un.S_addr;
    }

    if (macAddressPresent == ConfigKeyPresence::Absent && NetworkingMode == NetworkingMode::Bridged)
    {
        // Generate a random mac address if unspecified, so that the VM retains the same if restarted
        const std::independent_bits_engine<std::default_random_engine, 16, unsigned short> random;

        // independent_bits_engine doesn't support unsigned char on MSVC
        auto* macAddressShort = reinterpret_cast<unsigned short*>(MacAddress.data());
        std::generate(macAddressShort, macAddressShort + 3, random);

        // Clear the multicast bit
        MacAddress[0] &= ~1;
        // Set the locally generated bit.
        MacAddress[0] |= 2;
    }

    // Enable early boot logging if the debug console is enabled, unless explicitly disabled
    if (EnableDebugConsole || !DebugConsoleLogFile.empty())
    {
        if (earlyBootLoggingPresent == ConfigKeyPresence::Absent)
        {
            EnableEarlyBootLogging = true;
        }
    }

    if (CrashDumpFolder.empty() && MaxCrashDumpCount >= 0)
    {
        CrashDumpFolder = wsl::windows::common::filesystem::GetTempFolderPath(UserToken) / "wsl-crashes";
    }

    if (DefaultDistributionLocation.empty())
    {
        DefaultDistributionLocation = wsl::windows::common::filesystem::GetLocalAppDataPath(UserToken) / "wsl";
    }

    auto kernelModules =
        LoadDefaultKernelModules ? std::vector<std::wstring>{L"tun", L"ip_tables", L"br_netfilter"} : std::vector<std::wstring>{};

    if (!userKernelModules.empty())
    {
        for (const auto& e : wsl::shared::string::Split(userKernelModules, L','))
        {
            kernelModules.emplace_back(std::move(e));
        }
    }

    KernelModulesList = wsl::shared::string::Join(kernelModules, L',');
}

void wsl::core::Config::SaveNetworkingSettings(_In_opt_ HANDLE UserToken) const
try
{
    if (NetworkingMode != NetworkingMode::Nat)
    {
        return;
    }

    const auto machineKey = wsl::windows::common::registry::OpenLxssMachineKey(KEY_SET_VALUE);
    wsl::windows::common::registry::WriteString(machineKey.get(), nullptr, c_natGatewayAddress, NatGateway.c_str());
    wsl::windows::common::registry::WriteString(machineKey.get(), nullptr, c_natNetwork, NatNetwork.c_str());

    auto runAsUser = wil::impersonate_token(UserToken);
    const auto userKey = wsl::windows::common::registry::OpenLxssUserKey();
    wsl::windows::common::registry::WriteString(userKey.get(), nullptr, c_natIpAddress, NatIpAddress.c_str());
}
CATCH_LOG();

unsigned long wsl::core::Config::WriteConfigFile(_In_ LPCWSTR ConfigFilePath, _In_ ConfigKey KeyToWrite, _In_ bool RemoveKey)
{
    windows::common::ExecutionContext context(windows::common::ParseConfig);

    if (!ConfigFilePath)
    {
        return ERROR_INVALID_PARAMETER;
    }

    // Open file for reading & writing. This assumes the file exists.
    wil::unique_file ConfigFile(_wfopen(ConfigFilePath, L"r+t,ccs=UTF-8"));
    const auto win32Error = _doserrno;
    if (!ConfigFile && win32Error != ERROR_FILE_NOT_FOUND)
    {
        return win32Error;
    }

    // Since we aren't parsing in the config file, we don't need to pass in the known keys.
    std::vector<ConfigKey> keys{};
    std::wstring configFileOutput{};
    auto result = ::ParseConfigFile(
        keys, ConfigFile.get(), (CFG_SKIP_INVALID_LINES | CFG_SKIP_UNKNOWN_VALUES), ConfigFilePath, configFileOutput, KeyToWrite, RemoveKey);
    if (result != 0)
    {
        return ERROR_READ_FAULT;
    }

    // If the config file didn't exist/wasn't opened, open it for writing.
    if (!ConfigFile)
    {
        ConfigFile.reset(_wfopen(ConfigFilePath, L"wt,ccs=UTF-8"));
        if (!ConfigFile)
        {
            return _doserrno;
        }
    }

    // Move file pointer to beginning of file, write out the new config file, and truncate the file.
    rewind(ConfigFile.get());
    result = fputws(configFileOutput.c_str(), ConfigFile.get());
    if (result == WEOF)
    {
        return ERROR_WRITE_FAULT;
    }

    const auto fileHandle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(ConfigFile.get())));
    if (SetEndOfFile(fileHandle) != TRUE)
    {
        return GetLastError();
    }

    return ERROR_SUCCESS;
}

#define VALIDATE_CONFIG_OPTION(_dependency, _setting, _value) \
    { \
        LOG_HR_IF(E_INVALIDARG, _dependency && (_setting != _value)); \
        _setting = _value; \
    }

void wsl::core::Config::Initialize(_In_opt_ HANDLE UserToken)
{
    // Determine the maximum number of processors that can be added to the VM.
    // If the user did not supply a processor count, use the maximum.
    MaximumProcessorCount = wsl::windows::common::wslutil::GetLogicalProcessorCount();
    if (ProcessorCount <= 0)
    {
        ProcessorCount = MaximumProcessorCount;
    }
    else if (ProcessorCount > MaximumProcessorCount)
    {
        EMIT_USER_WARNING(wsl::shared::Localization::MessageTooManyProcessors(ProcessorCount, MaximumProcessorCount));
        ProcessorCount = MaximumProcessorCount;
    }

    // Determine how much memory to add to the VM. If the user did not specify a value,
    // use 50% of host memory. Otherwise, ensure the value falls within 256MB and the total system memory.
    MEMORYSTATUSEX memInfo{sizeof(MEMORYSTATUSEX)};
    THROW_IF_WIN32_BOOL_FALSE(GlobalMemoryStatusEx(&memInfo));

    MaximumMemorySizeBytes = memInfo.ullTotalPhys;
    if (MemorySizeBytes == 0)
    {
        MemorySizeBytes = (MaximumMemorySizeBytes / 2);
    }
    else
    {
        MemorySizeBytes = std::max<UINT64>(MemorySizeBytes, (256 * _1MB));
        MemorySizeBytes = std::min<UINT64>(MemorySizeBytes, MaximumMemorySizeBytes);
    }

    // Use the user-defined swap size if one was specified; otherwise, set to 25%
    // the memory size rounded up to the nearest GB.
    //
    // N.B. This heuristic is modeled after Red Hat and Ubuntu's recommended swap size.
    if (SwapSizeBytes == UINT64_MAX)
    {
        SwapSizeBytes = ((MemorySizeBytes / 4 + _1GB - 1) & ~(_1GB - 1));
    }

    // Apply machine-wide policies to the configuration.
    auto key = wsl::windows::policies::OpenPoliciesKey();
    auto applyOverride = [&key](LPCWSTR ValueName, LPCWSTR SettingName, auto& value) {
        if (value != std::remove_reference_t<decltype(value)>{} && !wsl::windows::policies::IsFeatureAllowed(key.get(), ValueName))
        {
            value = std::remove_reference_t<decltype(value)>{};
            EMIT_USER_WARNING(wsl::shared::Localization::MessageSettingOverriddenByPolicy(SettingName));
        }
    };

    applyOverride(wsl::windows::policies::c_allowCustomKernelUserSetting, L"wsl2.kernel", KernelPath);
    applyOverride(wsl::windows::policies::c_allowCustomKernelUserSetting, L"wsl2.kernelModules", KernelModulesPath);
    applyOverride(wsl::windows::policies::c_allowCustomSystemDistroUserSetting, L"wsl2.systemDistro", SystemDistroPath);
    applyOverride(wsl::windows::policies::c_allowCustomKernelCommandLineUserSetting, L"wsl2.kernelCommandLine", KernelCommandLine);
    applyOverride(wsl::windows::policies::c_allowKernelDebuggingUserSetting, L"wsl2.kernelDebugPort", KernelDebugPort);
    applyOverride(wsl::windows::policies::c_allowNestedVirtualizationUserSetting, L"wsl2.nestedVirtualization", EnableNestedVirtualization);

    if (!wsl::windows::policies::IsFeatureAllowed(key.get(), wsl::windows::policies::c_allowDebugShellUserSetting))
    {
        // N.B. The warning for debug shell is handled in wsl.exe.
        EnableDebugShell = false;
    }

    // Read the policy key for default networking mode.
    auto defaultNetworkingMode = wsl::core::NetworkingMode::Nat;
    const auto setting = wsl::windows::policies::GetPolicyValue(key.get(), wsl::windows::policies::c_defaultNetworkingMode);
    if (setting.has_value())
    {
        switch (setting.value())
        {
        case wsl::core::NetworkingMode::None:
        case wsl::core::NetworkingMode::Nat:
        case wsl::core::NetworkingMode::Mirrored:
        case wsl::core::NetworkingMode::VirtioProxy:
            defaultNetworkingMode = static_cast<wsl::core::NetworkingMode>(setting.value());
            break;

        case wsl::core::NetworkingMode::Bridged: // Bridged requires additional configuration.
        default:
            LOG_HR_MSG(E_UNEXPECTED, "Invalid default networking mode: %d", setting.value());
            break;
        }
    }

    // Determine if the user is allowed to override the networking mode.
    //
    // N.B. User can always disable networking entirely.
    if (NetworkingModePresence == ConfigKeyPresence::Present)
    {
        if ((!wsl::windows::policies::IsFeatureAllowed(key.get(), wsl::windows::policies::c_allowCustomNetworkingModeUserSetting)) &&
            (NetworkingMode != wsl::core::NetworkingMode::None) && (NetworkingMode != defaultNetworkingMode))
        {
            NetworkingMode = defaultNetworkingMode;
            EMIT_USER_WARNING(wsl::shared::Localization::MessageSettingOverriddenByPolicy(L"wsl2.networkingMode"));
        }
    }
    else
    {
        NetworkingMode = defaultNetworkingMode;
    }

    // Mirrored mode has Hyper-V Firewall always on - we ignore the local setting regardless in this case.
    if (NetworkingMode != wsl::core::NetworkingMode::Mirrored)
    {
        if (!FirewallConfig.Enabled() &&
            !wsl::windows::policies::IsFeatureAllowed(key.get(), wsl::windows::policies::c_allowCustomFirewallUserSetting))
        {
            FirewallConfig.Enable();
            EMIT_USER_WARNING(wsl::shared::Localization::MessageSettingOverriddenByPolicy(L"wsl2.firewall"));
        }
    }

    // Load NAT configuration from the registry.
    if (NetworkingMode == wsl::core::NetworkingMode::Nat)
    {
        try
        {
            const auto machineKey = wsl::windows::common::registry::OpenLxssMachineKey();
            NatGateway = wsl::windows::common::registry::ReadString(machineKey.get(), nullptr, c_natGatewayAddress, L"");
            NatNetwork = wsl::windows::common::registry::ReadString(machineKey.get(), nullptr, c_natNetwork, L"");

            auto runAsUser = wil::impersonate_token(UserToken);
            const auto userKey = wsl::windows::common::registry::OpenLxssUserKey();
            NatIpAddress = wsl::windows::common::registry::ReadString(userKey.get(), nullptr, c_natIpAddress, L"");
        }
        CATCH_LOG()
    }

    // Due to an issue with Global Secure Access Client, do not use DNS tunneling if the service is present.
    if (EnableDnsTunneling)
    {
        try
        {
            // Open a handle to the service control manager and check if the inbox service is registered.
            const wil::unique_schandle manager{OpenSCManager(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE)};
            THROW_LAST_ERROR_IF(!manager);

            // Check if the service is running.
            const wil::unique_schandle service{OpenServiceW(manager.get(), L"GlobalSecureAccessTunnelingService", SERVICE_QUERY_STATUS)};
            if (service)
            {
                SERVICE_STATUS status;
                THROW_IF_WIN32_BOOL_FALSE(QueryServiceStatus(service.get(), &status));

                if (status.dwCurrentState != SERVICE_STOPPED)
                {
                    if (DnsTunnelingConfigPresence == ConfigKeyPresence::Present)
                    {
                        EMIT_USER_WARNING(wsl::shared::Localization::MessageDnsTunnelingDisabled());
                    }

                    EnableDnsTunneling = false;
                }
            }
        }
        CATCH_LOG()
    }

    // Ensure that settings are consistent (disable features that require other features that are not present).
    if (EnableSafeMode)
    {
        EMIT_USER_WARNING(wsl::shared::Localization::MessageSafeModeEnabled());
        VALIDATE_CONFIG_OPTION(EnableSafeMode, EnableHostFileSystemAccess, false);
        VALIDATE_CONFIG_OPTION(EnableSafeMode, EnableNestedVirtualization, false);
        VALIDATE_CONFIG_OPTION(EnableSafeMode, EnableHardwarePerformanceCounters, false);
        VALIDATE_CONFIG_OPTION(EnableSafeMode, EnableGpuSupport, false);
        VALIDATE_CONFIG_OPTION(EnableSafeMode, EnableVirtio, false);
        VALIDATE_CONFIG_OPTION(EnableSafeMode, EnableGuiApps, false);
        VALIDATE_CONFIG_OPTION(EnableSafeMode, SwapSizeBytes, 0);
        VALIDATE_CONFIG_OPTION(EnableSafeMode, KernelPath, std::filesystem::path{});
        VALIDATE_CONFIG_OPTION(EnableSafeMode, KernelModulesPath, std::filesystem::path{});
        VALIDATE_CONFIG_OPTION(EnableSafeMode, NetworkingMode, NetworkingMode::None);
        VALIDATE_CONFIG_OPTION(EnableSafeMode, EnableDnsTunneling, false);
        VALIDATE_CONFIG_OPTION(EnableSafeMode, EnableAutoProxy, false);
    }

    if (!EnableVirtio)
    {
        VALIDATE_CONFIG_OPTION(!EnableVirtio, EnableVirtio9p, false);
        VALIDATE_CONFIG_OPTION(!EnableVirtio, EnableVirtioFs, false);
    }

    if (EnableVirtio9p)
    {
        EMIT_USER_WARNING(wsl::shared::Localization::MessageConfigVirtio9pDisabled());
        EnableVirtio9p = false;
    }

    if (NetworkingMode != NetworkingMode::Nat && NetworkingMode != NetworkingMode::Mirrored)
    {
        VALIDATE_CONFIG_OPTION((NetworkingMode != NetworkingMode::Nat && NetworkingMode != NetworkingMode::Mirrored), EnableDnsTunneling, false);
    }

    if (!EnableDnsTunneling)
    {
        VALIDATE_CONFIG_OPTION(!EnableDnsTunneling, BestEffortDnsParsing, false);
        VALIDATE_CONFIG_OPTION(!EnableDnsTunneling, DnsTunnelingIpAddress, std::optional<uint32_t>{});
    }

    if (NetworkingMode != NetworkingMode::Mirrored)
    {
        VALIDATE_CONFIG_OPTION((NetworkingMode != NetworkingMode::Mirrored), IgnoredPorts, std::set<uint16_t>{});
        VALIDATE_CONFIG_OPTION((NetworkingMode != NetworkingMode::Mirrored), EnableHostAddressLoopback, false);
    }
}

GUID wsl::core::Config::NatNetworkId() const noexcept
{
    // Identifier for the WSL virtual network: {b95d0c5e-57d4-412b-b571-18a81a16e005}
    static constexpr GUID c_networkId = {0xb95d0c5e, 0x57d4, 0x412b, {0xb5, 0x71, 0x18, 0xa8, 0x1a, 0x16, 0xe0, 0x05}};

    // Identifier for the WSL virtual network with Hyper-v firewall enabled: {790e58b4-7939-4434-9358-89ae7ddbe87e}
    static constexpr GUID c_networkWithFirewallId = {0x790e58b4, 0x7939, 0x4434, {0x93, 0x58, 0x89, 0xae, 0x7d, 0xdb, 0xe8, 0x7e}};

    return FirewallConfig.Enabled() ? c_networkWithFirewallId : c_networkId;
}

LPCWSTR wsl::core::Config::NatNetworkName() const noexcept
{
    static constexpr auto c_networkName = L"WSL";
    static constexpr auto c_networkWithFirewallName = L"WSL (Hyper-V firewall)";
    return FirewallConfig.Enabled() ? c_networkWithFirewallName : c_networkName;
}

void wsl::core::FirewallConfiguration::Enable() noexcept
{
    VmCreatorId = wsl::core::networking::c_wslFirewallVmCreatorId;
    DefaultLoopbackPolicy = FirewallAction::Allow;
    Rules = wsl::core::networking::MakeDefaultFirewallRuleConfiguration(networking::c_wslFirewallVmCreatorId);
}

void wsl::core::FirewallConfiguration::reset() noexcept
{
    VmCreatorId.reset();
    Rules.clear();
    DefaultLoopbackPolicy = FirewallAction::Invalid;
}

bool wsl::core::FirewallConfiguration::Enabled() const noexcept
{
    return VmCreatorId.has_value();
}
