/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    wslpolicies.h

Abstract:

    This file contains a helpers for querying WSL policies.

--*/

#pragma once

#include "registry.hpp"

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
inline constexpr auto c_allowWSLContainer = L"AllowWSLContainer";
inline constexpr auto c_wslContainerRegistryAllowlist = L"WSLContainerRegistryAllowlist";

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

// Opens the WSLContainerRegistryAllowlist sub-key under the supplied policies key for
// read-only enumeration. Returns an empty handle when the policy is not configured (sub-key
// absent) or the parent key is null.
inline wil::unique_hkey OpenRegistryAllowlistKey(HKEY policiesKey)
{
    if (policiesKey == nullptr)
    {
        return {};
    }

    wil::unique_hkey subKey;
    const auto result = RegOpenKeyExW(policiesKey, c_wslContainerRegistryAllowlist, 0, KEY_READ, &subKey);
    if (result == ERROR_PATH_NOT_FOUND || result == ERROR_FILE_NOT_FOUND)
    {
        return {};
    }

    LOG_IF_WIN32_ERROR(result);
    return subKey;
}

// Returns the REG_SZ data of every value under the WSLContainerRegistryAllowlist sub-key
// (one entry per configured registry hostname). The ADMX uses
// `<list valuePrefix="AllowedRegistry"/>`, which the GP editor materialises by writing one
// REG_SZ value per entry: each value is named `AllowedRegistry1`, `AllowedRegistry2`, ... and
// the value's data is the actual hostname; the value names are therefore ignored here. Schema
// reference: https://learn.microsoft.com/en-us/previous-versions/windows/desktop/Policy/element-list
//
// Returns an empty list when the sub-key has no values or on any enumeration failure; either
// case means no effective restriction is in place.
inline std::vector<std::wstring> EnumerateRegistryAllowlist(HKEY subKey)
try
{
    std::vector<std::wstring> entries;
    if (subKey == nullptr)
    {
        return entries;
    }

    for (auto& [name, value] : wsl::windows::common::registry::EnumStringValues(subKey))
    {
        // Skip empty entries so a stray blank list item in the GP editor doesn't make the
        // allowlist non-empty (which would otherwise deny every registry).
        if (value.empty())
        {
            continue;
        }

        entries.emplace_back(std::move(value));
    }

    return entries;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

// Evaluates the WSLContainerRegistryAllowlist policy for `server`. The policy only restricts
// traffic when the sub-key exists and contains at least one entry. With the sub-key absent or
// empty, every server is allowed; otherwise the server is allowed only when it case-insensitively
// matches one of the allowlist entries.
inline bool IsRegistryAllowed(HKEY policiesKey, std::wstring_view server)
{
    auto subKey = OpenRegistryAllowlistKey(policiesKey);
    if (!subKey)
    {
        return true;
    }

    const auto entries = EnumerateRegistryAllowlist(subKey.get());
    if (entries.empty())
    {
        return true;
    }

    if (server.empty())
    {
        return true;
    }

    const std::wstring target{server};
    for (const auto& entry : entries)
    {
        if (_wcsicmp(entry.c_str(), target.c_str()) == 0)
        {
            return true;
        }
    }
    return false;
}

// Returns true when the WSLContainerRegistryAllowlist policy is in effect (sub-key present
// with at least one entry). Used by callers (e.g., `wslc image build`) that cannot attribute
// traffic to a specific registry and must therefore refuse the operation whenever any
// allowlist restriction is active.
inline bool HasRegistryAllowlist(HKEY policiesKey)
{
    auto subKey = OpenRegistryAllowlistKey(policiesKey);
    return subKey && !EnumerateRegistryAllowlist(subKey.get()).empty();
}

} // namespace wsl::windows::policies