/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslpolicies.h

Abstract:

    This file contains a helpers for querying WSL policies.

--*/

#pragma once

#define ROOT_POLICIES_KEY L"Software\\Policies"

namespace wsl::windows::policies {
inline constexpr auto c_registryKey = ROOT_POLICIES_KEY L"\\WSL";
inline constexpr auto c_allowInboxWSL = L"AllowInboxWSL";
inline constexpr auto c_allowWSL = L"AllowWSL";
inline constexpr auto c_allowWSL1 = L"AllowWSL1";
inline constexpr auto c_allowCustomKernelUserSetting = L"AllowKernelUserSetting";
inline constexpr auto c_allowCustomSystemDistroUserSetting = L"AllowSystemDistroUserSetting";
inline constexpr auto c_allowCustomKernelCommandLineUserSetting = L"AllowKernelCommandLineUserSetting";
inline constexpr auto c_allowDebugShellUserSetting = L"AllowDebugShell";
inline constexpr auto c_allowNestedVirtualizationUserSetting = L"AllowNestedVirtualization";
inline constexpr auto c_allowKernelDebuggingUserSetting = L"AllowKernelDebugUserSetting";
inline constexpr auto c_allowDiskMount = L"AllowDiskMount";
inline constexpr auto c_allowCustomNetworkingModeUserSetting = L"AllowNetworkingModeUserSetting";
inline constexpr auto c_allowCustomFirewallUserSetting = L"AllowFirewallUserSetting";
inline constexpr auto c_defaultNetworkingMode = L"DefaultNetworkingMode";

inline wil::unique_hkey CreatePoliciesKey(DWORD desiredAccess)
{
    wil::unique_hkey key;
    LOG_IF_WIN32_ERROR(
        RegCreateKeyExW(HKEY_LOCAL_MACHINE, c_registryKey, 0, nullptr, REG_OPTION_NON_VOLATILE, desiredAccess, nullptr, &key, nullptr));

    return key;
}

inline std::optional<DWORD> GetPolicyValue(HKEY key, LPCWSTR name)
try
{
    if (key == nullptr)
    {
        return std::nullopt;
    }

    DWORD value = 0;
    DWORD size = sizeof(value);
    const LONG result = RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (result == ERROR_PATH_NOT_FOUND || result == ERROR_FILE_NOT_FOUND)
    {
        return std::nullopt;
    }

    THROW_IF_WIN32_ERROR(result);

    return value;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION_MSG("Error reading the policy value: %ls", name);
    return std::nullopt;
}

inline bool IsFeatureAllowed(HKEY key, LPCWSTR name)
try
{
    const auto policy = GetPolicyValue(key, name);
    if (!policy.has_value())
    {
        return true;
    }

    const auto value = policy.value();
    THROW_HR_IF_MSG(E_UNEXPECTED, value != 0 && value != 1, "Invalid value for policy: %ls: %lu", name, value);

    return value == 1;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return true;
}

inline wil::unique_hkey OpenPoliciesKey()
{
    wil::unique_hkey key;
    const auto result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, c_registryKey, 0, KEY_READ, &key);
    if (result == ERROR_PATH_NOT_FOUND || result == ERROR_FILE_NOT_FOUND)
    {
        // N.B. Return an empty result if the registry key doesn't exist to make it easier
        // to check for policies without having a special code path for this case.
        return {};
    }

    LOG_IF_WIN32_ERROR(result);
    return key;
}

} // namespace wsl::windows::policies