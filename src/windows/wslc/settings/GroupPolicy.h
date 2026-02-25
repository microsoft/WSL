/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    GroupPolicy.h

Abstract:

    Group Policy integration for wslc settings. Reads toggle and value policies
    from the registry under HKLM\Software\Policies\WSL\WSLC.

--*/
#pragma once

#include "SettingDefinitions.h"
#include <optional>

namespace wsl::windows::wslc::settings {

enum class PolicyState
{
    NotConfigured,
    Enabled,
    Disabled
};

// Toggle policy names (maps to registry DWORD values: 0=disabled, 1=enabled, absent=NotConfigured).
struct TogglePolicy
{
    static constexpr LPCWSTR AllowSettings = L"AllowSettings";
    static constexpr LPCWSTR AllowCustomNetworkingMode = L"AllowCustomNetworkingMode";
};

// Value policy template mapping.
template <ValuePolicy P>
struct ValuePolicyMapping;

template <>
struct ValuePolicyMapping<ValuePolicy::MaxCpuCount>
{
    using value_t = DWORD;
    static constexpr LPCWSTR ValueName = L"MaxCpuCount";
};

template <>
struct ValuePolicyMapping<ValuePolicy::MaxMemoryMb>
{
    using value_t = DWORD;
    static constexpr LPCWSTR ValueName = L"MaxMemoryMb";
};

template <>
struct ValuePolicyMapping<ValuePolicy::DefaultNetworkingMode>
{
    using value_t = DWORD;
    static constexpr LPCWSTR ValueName = L"DefaultNetworkingMode";
};

struct GroupPolicy
{
    static const GroupPolicy& Instance();

    // Toggle policy check.
    PolicyState GetState(LPCWSTR policyName) const;
    bool IsEnabled(LPCWSTR policyName) const;

    // Value policy retrieval.
    template <ValuePolicy P>
    std::optional<typename ValuePolicyMapping<P>::value_t> GetValue() const
    {
        return ReadDwordValue(ValuePolicyMapping<P>::ValueName);
    }

#ifndef WSLC_DISABLE_TEST_HOOKS
    static void OverrideInstance(const GroupPolicy* gp);
    static void ResetInstance();
#endif

private:
    GroupPolicy();
    std::optional<DWORD> ReadDwordValue(LPCWSTR valueName) const;

    wil::unique_hkey m_key;
};

inline const GroupPolicy& GroupPolicies()
{
    return GroupPolicy::Instance();
}

} // namespace wsl::windows::wslc::settings
