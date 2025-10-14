/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreConfigInterface.cpp

Abstract:

    This file contains the WSL Core Config Interface class interface definition.

--*/

#include <WslCoreConfigInterface.h>
#include "WslCoreConfig.h"
#include <helpers.hpp>

using namespace wsl::shared::string;
using namespace wsl::core;

static_assert(NetworkingConfiguration::None == static_cast<int32_t>(wsl::core::NetworkingMode::None));
static_assert(NetworkingConfiguration::Nat == static_cast<int32_t>(wsl::core::NetworkingMode::Nat));
static_assert(NetworkingConfiguration::Bridged == static_cast<int32_t>(wsl::core::NetworkingMode::Bridged));
static_assert(NetworkingConfiguration::Mirrored == static_cast<int32_t>(wsl::core::NetworkingMode::Mirrored));
static_assert(NetworkingConfiguration::VirtioProxy == static_cast<int32_t>(wsl::core::NetworkingMode::VirtioProxy));

static_assert(MemoryReclaimConfiguration::Disabled == static_cast<int32_t>(wsl::core::MemoryReclaimMode::Disabled));
static_assert(MemoryReclaimConfiguration::Gradual == static_cast<int32_t>(wsl::core::MemoryReclaimMode::Gradual));
static_assert(MemoryReclaimConfiguration::DropCache == static_cast<int32_t>(wsl::core::MemoryReclaimMode::DropCache));

struct WslConfig
{
    WslConfig() = default;

    WslConfig(const wchar_t* wslConfigFilePath) :
        ConfigFilePath(wslConfigFilePath == nullptr ? std::filesystem::path() : wslConfigFilePath), Config(wslConfigFilePath)
    {
    }

    std::filesystem::path ConfigFilePath;
    wsl::core::Config Config;
    std::wstring IgnoredPortsStr;
};

const wchar_t* GetWslConfigFilePath()
{
    static std::filesystem::path g_wslConfigFilePath;
    static std::once_flag flag;
    std::call_once(flag, [&]() { g_wslConfigFilePath = wsl::windows::common::helpers::GetWslConfigPath(nullptr); });

    return g_wslConfigFilePath.c_str();
}

WslConfig_t CreateWslConfig(const wchar_t* wslConfigFilePath)
{
    return new WslConfig(wslConfigFilePath);
}

void FreeWslConfig(WslConfig_t wslConfig)
{
    if (wslConfig)
    {
        delete wslConfig;
    }
}

WslConfigSetting GetWslConfigSetting(WslConfig_t wslConfig, WslConfigEntry wslConfigEntry)
{
    if (wslConfig == nullptr)
    {
        return {.ConfigEntry = WslConfigEntry::NoEntry};
    }

    WslConfigSetting wslConfigSetting{.ConfigEntry = wslConfigEntry};
    switch (wslConfigEntry)
    {
    case NoEntry:
        // In addition to returning this entry type on error (e.g. invalid WslConfig_t parameter),
        // the caller can request this entry type returned to initialize an empty WslConfigSetting object.
        // The caller can then use the returned object in future calls to this function. The intention
        // of this is an interop scenario where an auto-generated interop layer manages unmanaged memory
        // (the returned WslConfigSetting object in this case) and is unable or doesn't generate the interop
        // code to create such an object itself.
        return wslConfigSetting;
    case ProcessorCount:
        static_assert(std::is_same<decltype(wslConfigSetting.Int32Value), decltype(wslConfig->Config.ProcessorCount)>::value);
        wslConfigSetting.Int32Value = wslConfig->Config.ProcessorCount;
        break;
    case MemorySizeBytes:
        static_assert(std::is_same<decltype(wslConfigSetting.UInt64Value), decltype(wslConfig->Config.MemorySizeBytes)>::value);
        wslConfigSetting.UInt64Value = wslConfig->Config.MemorySizeBytes;
        break;
    case SwapSizeBytes:
        static_assert(std::is_same<decltype(wslConfigSetting.UInt64Value), decltype(wslConfig->Config.SwapSizeBytes)>::value);
        wslConfigSetting.UInt64Value = wslConfig->Config.SwapSizeBytes;
        return wslConfigSetting;
    case SwapFilePath:
        static_assert(std::is_same<decltype(wslConfigSetting.StringValue), decltype(wslConfig->Config.SwapFilePath.c_str())>::value);
        wslConfigSetting.StringValue = wslConfig->Config.SwapFilePath.c_str();
        break;
    case VhdSizeBytes:
        static_assert(std::is_same<decltype(wslConfigSetting.UInt64Value), decltype(wslConfig->Config.VhdSizeBytes)>::value);
        wslConfigSetting.UInt64Value = wslConfig->Config.VhdSizeBytes;
        break;
    case Networking:
        wslConfigSetting.NetworkingConfigurationValue = static_cast<NetworkingConfiguration>(wslConfig->Config.NetworkingMode);
        break;
    case FirewallEnabled:
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.FirewallConfig.Enabled())>::value);
        wslConfigSetting.BoolValue = wslConfig->Config.FirewallConfig.Enabled();
        break;
    case IgnoredPorts:
    {
        // The IgnoredPorts member is stored as a set of 16-bit unsigned integers.
        // These are converted back to a string here for the caller to consume.
        wslConfig->IgnoredPortsStr.clear();
        std::vector<uint16_t> ignoredPorts{wslConfig->Config.IgnoredPorts.begin(), wslConfig->Config.IgnoredPorts.end()};
        std::sort(ignoredPorts.begin(), ignoredPorts.end());

        wslConfig->IgnoredPortsStr = Join(ignoredPorts, L',');

        static_assert(std::is_same<decltype(wslConfigSetting.StringValue), decltype(wslConfig->IgnoredPortsStr.c_str())>::value);
        wslConfigSetting.StringValue = wslConfig->IgnoredPortsStr.c_str();
    }
    break;
    case LocalhostForwardingEnabled:
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableLocalhostRelay)>::value);
        wslConfigSetting.BoolValue = wslConfig->Config.EnableLocalhostRelay;
        break;
    case HostAddressLoopbackEnabled:
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableHostAddressLoopback)>::value);
        wslConfigSetting.BoolValue = wslConfig->Config.EnableHostAddressLoopback;
        break;
    case AutoProxyEnabled:
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableAutoProxy)>::value);
        wslConfigSetting.BoolValue = wslConfig->Config.EnableAutoProxy;
        break;
    case InitialAutoProxyTimeout:
        static_assert(std::is_same<decltype(wslConfigSetting.Int32Value), decltype(wslConfig->Config.InitialAutoProxyTimeout)>::value);
        wslConfigSetting.Int32Value = wslConfig->Config.InitialAutoProxyTimeout;
        break;
    case DNSProxyEnabled:
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableDnsProxy)>::value);
        wslConfigSetting.BoolValue = wslConfig->Config.EnableDnsProxy;
        break;
    case DNSTunnelingEnabled:
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableDnsTunneling)>::value);
        wslConfigSetting.BoolValue = wslConfig->Config.EnableDnsTunneling;
        break;
    case BestEffortDNSParsingEnabled:
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.BestEffortDnsParsing)>::value);
        wslConfigSetting.BoolValue = wslConfig->Config.BestEffortDnsParsing;
        break;
    case AutoMemoryReclaim:
        wslConfigSetting.MemoryReclaimModeValue = static_cast<MemoryReclaimConfiguration>(wslConfig->Config.MemoryReclaim);
        break;
    case GUIApplicationsEnabled:
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableGuiApps)>::value);
        wslConfigSetting.BoolValue = wslConfig->Config.EnableGuiApps;
        break;
    case NestedVirtualizationEnabled:
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableNestedVirtualization)>::value);
        wslConfigSetting.BoolValue = wslConfig->Config.EnableNestedVirtualization;
        break;
    case SafeModeEnabled:
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableSafeMode)>::value);
        wslConfigSetting.BoolValue = wslConfig->Config.EnableSafeMode;
        break;
    case SparseVHDEnabled:
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableSparseVhd)>::value);
        wslConfigSetting.BoolValue = wslConfig->Config.EnableSparseVhd;
        break;
    case VMIdleTimeout:
        static_assert(std::is_same<decltype(wslConfigSetting.Int32Value), decltype(wslConfig->Config.VmIdleTimeout)>::value);
        wslConfigSetting.Int32Value = wslConfig->Config.VmIdleTimeout;
        break;
    case DebugConsoleEnabled:
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableDebugConsole)>::value);
        wslConfigSetting.BoolValue = wslConfig->Config.EnableDebugConsole;
        break;
    case HardwarePerformanceCountersEnabled:
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableHardwarePerformanceCounters)>::value);
        wslConfigSetting.BoolValue = wslConfig->Config.EnableHardwarePerformanceCounters;
        break;
    case KernelPath:
        static_assert(std::is_same<decltype(wslConfigSetting.StringValue), decltype(wslConfig->Config.KernelPath.c_str())>::value);
        wslConfigSetting.StringValue = wslConfig->Config.KernelPath.c_str();
        break;
    case SystemDistroPath:
        static_assert(std::is_same<decltype(wslConfigSetting.StringValue), decltype(wslConfig->Config.SystemDistroPath.c_str())>::value);
        wslConfigSetting.StringValue = wslConfig->Config.SystemDistroPath.c_str();
        break;
    case KernelModulesPath:
        static_assert(std::is_same<decltype(wslConfigSetting.StringValue), decltype(wslConfig->Config.KernelModulesPath.c_str())>::value);
        wslConfigSetting.StringValue = wslConfig->Config.KernelModulesPath.c_str();
        break;
    default:
        FAIL_FAST();
    }

    return wslConfigSetting;
}

// Template instantiation acts as a type check for the ValueType parameter (e.g. an
// implicit assert for newValue and outValue having the same type).
template <typename ValueType>
unsigned long SetWslConfigSetting(WslConfig_t wslConfig, const char* keyName, ValueType& defaultValue, ValueType& newValue, ValueType& outValue)
{
    ConfigKey configKey(keyName, newValue);
    const auto removeKey = defaultValue == newValue;
    const auto result = wslConfig->Config.WriteConfigFile(wslConfig->ConfigFilePath.c_str(), configKey, removeKey);
    if (result == 0)
    {
        outValue = newValue;
    }

    return result;
}

unsigned long SetWslConfigSetting(WslConfig_t wslConfig, const char* keyName, const wchar_t* defaultValue, const wchar_t* newValue, std::wstring& outValue)
{
    std::wstring newValueStr{newValue};
    const ConfigKey configKey(keyName, newValueStr);
    const auto removeKey = wsl::shared::string::IsEqual(defaultValue, newValueStr, true);
    const auto result = wslConfig->Config.WriteConfigFile(wslConfig->ConfigFilePath.c_str(), configKey, removeKey);
    if (result == 0)
    {
        outValue = std::move(newValueStr);
    }

    return result;
}

unsigned long SetWslConfigSetting(
    WslConfig_t wslConfig, const char* keyName, const std::filesystem::path& defaultValue, const wchar_t* newValue, std::filesystem::path& outValue)
{
    // Normalize the path.
    std::filesystem::path filePath;
    try
    {
        filePath = std::filesystem::path(newValue);
    }
    catch (const std::exception&)
    {
        return ERROR_INVALID_PARAMETER;
    }

    // The WSL config file needs to have backslashes escaped for file paths.
    // Assuming these are valid Windows Paths, replace any backslashes with double backslashes.
    auto newValueStr = std::regex_replace(filePath.wstring(), std::wregex(L"\\\\"), L"\\\\");
    const ConfigKey configKey(keyName, newValueStr);
    const auto removeKey = defaultValue == newValue;
    const auto result = wslConfig->Config.WriteConfigFile(wslConfig->ConfigFilePath.c_str(), configKey, removeKey);
    if (result == 0)
    {
        outValue = newValue;
    }

    return result;
}

unsigned long SetWslConfigSetting(
    WslConfig_t wslConfig, const char* keyName, const MemoryString& defaultValue, const MemoryString& newValue, unsigned __int64& outValue)
{
    const ConfigKey configKey(keyName, newValue);
    const auto removeKey = defaultValue.m_value == newValue.m_value;
    const auto result = wslConfig->Config.WriteConfigFile(wslConfig->ConfigFilePath.c_str(), configKey, removeKey);
    if (result == 0)
    {
        outValue = newValue.m_value;
    }

    return result;
}

unsigned long SetWslConfigSetting(WslConfig_t wslConfig, WslConfigSetting wslConfigSetting)
{
    if (wslConfig == nullptr)
    {
        return ERROR_INVALID_PARAMETER;
    }

    // Create a Config object with the default initialized values.
    wsl::core::Config defaultConfig(nullptr);

    // The following scope exit is intended to re-initialize/update the Config object after a change is made
    // upon a successful write to the config file and updating of the corresponding member of the Config object.
    // Depending on the member and value updated, the value itself or others may change as well.
    auto initializeConfig = wil::scope_exit([&] { wslConfig->Config.Initialize(); });

    switch (wslConfigSetting.ConfigEntry)
    {
    case ProcessorCount:
        return SetWslConfigSetting(
            wslConfig, ConfigSetting::Processors, defaultConfig.ProcessorCount, wslConfigSetting.Int32Value, wslConfig->Config.ProcessorCount);
    case MemorySizeBytes:
        return SetWslConfigSetting(
            wslConfig,
            ConfigSetting::Memory,
            MemoryString(defaultConfig.MemorySizeBytes),
            MemoryString(wslConfigSetting.UInt64Value),
            wslConfig->Config.MemorySizeBytes);
    case SwapSizeBytes:
        return SetWslConfigSetting(
            wslConfig,
            ConfigSetting::Swap,
            MemoryString(defaultConfig.SwapSizeBytes),
            MemoryString(wslConfigSetting.UInt64Value),
            wslConfig->Config.SwapSizeBytes);
    case SwapFilePath:
    {
        if (wslConfigSetting.StringValue == nullptr)
        {
            return ERROR_INVALID_PARAMETER;
        }

        return SetWslConfigSetting(
            wslConfig, ConfigSetting::SwapFile, defaultConfig.SwapFilePath, wslConfigSetting.StringValue, wslConfig->Config.SwapFilePath);
    }
    case VhdSizeBytes:
        return SetWslConfigSetting(
            wslConfig,
            ConfigSetting::DefaultVhdSize,
            MemoryString(defaultConfig.VhdSizeBytes),
            MemoryString(wslConfigSetting.UInt64Value),
            wslConfig->Config.VhdSizeBytes);
    case Networking:
    {
        wsl::core::NetworkingMode networkingConfiguration{static_cast<wsl::core::NetworkingMode>(wslConfigSetting.NetworkingConfigurationValue)};
        ConfigKey key({ConfigSetting::NetworkingMode, ConfigSetting::Experimental::NetworkingMode}, wsl::core::NetworkingModes, networkingConfiguration);
        const auto removeKey = defaultConfig.NetworkingMode == networkingConfiguration;
        const auto result = wslConfig->Config.WriteConfigFile(wslConfig->ConfigFilePath.c_str(), key, removeKey);
        if (result == 0)
        {
            wslConfig->Config.NetworkingMode = networkingConfiguration;
            wslConfig->Config.NetworkingModePresence = removeKey ? ConfigKeyPresence::Absent : ConfigKeyPresence::Present;
        }
        return result;
    }
    case FirewallEnabled:
    {
        ConfigKey key({ConfigSetting::Firewall, ConfigSetting::Experimental::Firewall}, wslConfigSetting.BoolValue);
        const auto removeKey = defaultConfig.FirewallConfig.Enabled() == wslConfigSetting.BoolValue;
        const auto result = wslConfig->Config.WriteConfigFile(wslConfig->ConfigFilePath.c_str(), key, removeKey);
        if (result == 0)
        {
            if (wslConfigSetting.BoolValue)
            {
                wslConfig->Config.FirewallConfig.Enable();
            }
            else
            {
                wslConfig->Config.FirewallConfig.reset();
            }

            wslConfig->Config.FirewallConfigPresence = removeKey ? ConfigKeyPresence::Absent : ConfigKeyPresence::Present;
        }

        return result;
    }
    case IgnoredPorts:
    {
        if (wslConfigSetting.StringValue == nullptr)
        {
            return ERROR_INVALID_PARAMETER;
        }

        std::string ignoredPortsUtf8;
        if (FAILED(wil::ResultFromException(
                [&]() { ignoredPortsUtf8 = wsl::shared::string::WideToMultiByte(wslConfigSetting.StringValue); })))
        {
            return ERROR_INVALID_PARAMETER;
        }

        // IgnoredPorts is unique compared to other settings as it parses a string into a set of 16-bit unsigned integers.
        // In this case, write out the ignored ports as a string. On success, parse the string into a set of 16-bit unsigned
        // integers and update the IgnoredPorts member of the Config object.
        static_assert(std::is_same<decltype(wslConfigSetting.StringValue), decltype(wslConfig->IgnoredPortsStr.c_str())>::value);
        const auto ignoredPortsKeyName = ConfigSetting::Experimental::IgnoredPorts;

        const std::vector<uint16_t> defaultIgnoredPorts{defaultConfig.IgnoredPorts.begin(), defaultConfig.IgnoredPorts.end()};
        const std::wstring defaultIgnoredPortsStr = Join(defaultIgnoredPorts, L',');

        const auto result = SetWslConfigSetting(
            wslConfig, ignoredPortsKeyName, defaultIgnoredPortsStr.c_str(), wslConfigSetting.StringValue, wslConfig->IgnoredPortsStr);
        if (result == 0)
        {
            wslConfig->Config.IgnoredPorts.clear();
            auto parseIgnoredPorts = [&wslConfig](const char*, const char* value, const wchar_t*, unsigned long) {
                const auto ignoredPortsVector = Split(std::string{value}, ',');
                for (const auto& portString : ignoredPortsVector)
                {
                    int number = 0;
                    if (FAILED(wil::ResultFromException([&]() { number = std::stoi(portString); })) || (number <= 0 || number > USHRT_MAX))
                    {
                        continue;
                    }

                    wslConfig->Config.IgnoredPorts.insert(static_cast<uint16_t>(number));
                }
            };

            ConfigKey key(ignoredPortsKeyName, std::move(parseIgnoredPorts));
            key.Parse(ignoredPortsKeyName, ignoredPortsUtf8.c_str(), nullptr, 0);
        }

        return result;
    }
    case LocalhostForwardingEnabled:
    {
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableLocalhostRelay)>::value);
        ConfigKey key(ConfigSetting::LocalhostForwarding, wslConfigSetting.BoolValue);
        const auto removeKey = defaultConfig.EnableLocalhostRelay == wslConfigSetting.BoolValue;
        const auto result = wslConfig->Config.WriteConfigFile(wslConfig->ConfigFilePath.c_str(), key, removeKey);
        if (result == 0)
        {
            wslConfig->Config.EnableLocalhostRelay = wslConfigSetting.BoolValue;
            wslConfig->Config.LocalhostRelayConfigPresence = removeKey ? ConfigKeyPresence::Absent : ConfigKeyPresence::Present;
        }

        return result;
    }
    case HostAddressLoopbackEnabled:
        return SetWslConfigSetting(
            wslConfig,
            ConfigSetting::Experimental::HostAddressLoopback,
            defaultConfig.EnableHostAddressLoopback,
            wslConfigSetting.BoolValue,
            wslConfig->Config.EnableHostAddressLoopback);
    case AutoProxyEnabled:
    {
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableAutoProxy)>::value);
        ConfigKey key({ConfigSetting::AutoProxy, ConfigSetting::Experimental::AutoProxy}, wslConfigSetting.BoolValue);
        const auto removeKey = defaultConfig.EnableAutoProxy == wslConfigSetting.BoolValue;
        const auto result = wslConfig->Config.WriteConfigFile(wslConfig->ConfigFilePath.c_str(), key, removeKey);
        if (result == 0)
        {
            wslConfig->Config.EnableAutoProxy = wslConfigSetting.BoolValue;
        }

        return result;
    }
    case InitialAutoProxyTimeout:
        return SetWslConfigSetting(
            wslConfig,
            ConfigSetting::Experimental::InitialAutoProxyTimeout,
            defaultConfig.InitialAutoProxyTimeout,
            wslConfigSetting.Int32Value,
            wslConfig->Config.InitialAutoProxyTimeout);
    case DNSProxyEnabled:
        return SetWslConfigSetting(
            wslConfig, ConfigSetting::DnsProxy, defaultConfig.EnableDnsProxy, wslConfigSetting.BoolValue, wslConfig->Config.EnableDnsProxy);
    case DNSTunnelingEnabled:
    {
        static_assert(std::is_same<decltype(wslConfigSetting.BoolValue), decltype(wslConfig->Config.EnableDnsTunneling)>::value);
        ConfigKey key({ConfigSetting::DnsTunneling, ConfigSetting::Experimental::DnsTunneling}, wslConfigSetting.BoolValue);
        const auto removeKey = defaultConfig.EnableDnsTunneling == wslConfigSetting.BoolValue;
        const auto result = wslConfig->Config.WriteConfigFile(wslConfig->ConfigFilePath.c_str(), key, removeKey);
        if (result == 0)
        {
            wslConfig->Config.EnableDnsTunneling = wslConfigSetting.BoolValue;
            wslConfig->Config.DnsTunnelingConfigPresence = removeKey ? ConfigKeyPresence::Absent : ConfigKeyPresence::Present;
        }

        return result;
    }
    case BestEffortDNSParsingEnabled:
        return SetWslConfigSetting(
            wslConfig,
            ConfigSetting::Experimental::BestEffortDnsParsing,
            defaultConfig.BestEffortDnsParsing,
            wslConfigSetting.BoolValue,
            wslConfig->Config.BestEffortDnsParsing);
    case AutoMemoryReclaim:
    {
        wsl::core::MemoryReclaimMode memoryReclaimMode{static_cast<wsl::core::MemoryReclaimMode>(wslConfigSetting.MemoryReclaimModeValue)};
        ConfigKey key({ConfigSetting::Experimental::AutoMemoryReclaim}, wsl::core::MemoryReclaimModes, memoryReclaimMode);
        const auto removeKey = defaultConfig.MemoryReclaim == memoryReclaimMode;
        const auto result = wslConfig->Config.WriteConfigFile(wslConfig->ConfigFilePath.c_str(), key, removeKey);
        if (result == 0)
        {
            wslConfig->Config.MemoryReclaim = memoryReclaimMode;
        }

        return result;
    }
    case GUIApplicationsEnabled:
        return SetWslConfigSetting(
            wslConfig, ConfigSetting::GuiApplications, defaultConfig.EnableGuiApps, wslConfigSetting.BoolValue, wslConfig->Config.EnableGuiApps);
    case NestedVirtualizationEnabled:
        return SetWslConfigSetting(
            wslConfig,
            ConfigSetting::NestedVirtualization,
            defaultConfig.EnableNestedVirtualization,
            wslConfigSetting.BoolValue,
            wslConfig->Config.EnableNestedVirtualization);
    case SafeModeEnabled:
        return SetWslConfigSetting(
            wslConfig, ConfigSetting::SafeMode, defaultConfig.EnableSafeMode, wslConfigSetting.BoolValue, wslConfig->Config.EnableSafeMode);
    case SparseVHDEnabled:
        return SetWslConfigSetting(
            wslConfig,
            ConfigSetting::Experimental::SparseVhd,
            defaultConfig.EnableSparseVhd,
            wslConfigSetting.BoolValue,
            wslConfig->Config.EnableSparseVhd);
    case VMIdleTimeout:
        return SetWslConfigSetting(
            wslConfig, ConfigSetting::VmIdleTimeout, defaultConfig.VmIdleTimeout, wslConfigSetting.Int32Value, wslConfig->Config.VmIdleTimeout);
    case DebugConsoleEnabled:
        return SetWslConfigSetting(
            wslConfig,
            ConfigSetting::DebugConsole,
            defaultConfig.EnableDebugConsole,
            wslConfigSetting.BoolValue,
            wslConfig->Config.EnableDebugConsole);
    case HardwarePerformanceCountersEnabled:
        return SetWslConfigSetting(
            wslConfig,
            ConfigSetting::HardwarePerformanceCounters,
            defaultConfig.EnableHardwarePerformanceCounters,
            wslConfigSetting.BoolValue,
            wslConfig->Config.EnableHardwarePerformanceCounters);
    case KernelPath:
    {
        if (wslConfigSetting.StringValue == nullptr)
        {
            return ERROR_INVALID_PARAMETER;
        }

        return SetWslConfigSetting(
            wslConfig, ConfigSetting::Kernel, defaultConfig.KernelPath, wslConfigSetting.StringValue, wslConfig->Config.KernelPath);
    }
    case SystemDistroPath:
    {
        if (wslConfigSetting.StringValue == nullptr)
        {
            return ERROR_INVALID_PARAMETER;
        }

        return SetWslConfigSetting(
            wslConfig,
            ConfigSetting::SystemDistro,
            defaultConfig.SystemDistroPath,
            wslConfigSetting.StringValue,
            wslConfig->Config.SystemDistroPath);
    }
    case KernelModulesPath:
    {
        if (wslConfigSetting.StringValue == nullptr)
        {
            return ERROR_INVALID_PARAMETER;
        }

        return SetWslConfigSetting(
            wslConfig,
            ConfigSetting::KernelModules,
            defaultConfig.KernelModulesPath,
            wslConfigSetting.StringValue,
            wslConfig->Config.KernelModulesPath);
    }
    default:
        FAIL_FAST();
    }
}