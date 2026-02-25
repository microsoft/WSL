/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    GroupPolicy.cpp

Abstract:

    Group Policy implementation for wslc settings. Reads policies from the
    registry under HKLM\Software\Policies\WSL\WSLC.

--*/

#include <precomp.h>
#include "GroupPolicy.h"

namespace wsl::windows::wslc::settings {

static constexpr auto c_registryKey = L"Software\\Policies\\WSL\\WSLC";

static const GroupPolicy* s_override = nullptr;

const GroupPolicy& GroupPolicy::Instance()
{
    if (s_override)
    {
        return *s_override;
    }

    static GroupPolicy instance;
    return instance;
}

#ifndef WSLC_DISABLE_TEST_HOOKS
void GroupPolicy::OverrideInstance(const GroupPolicy* gp)
{
    s_override = gp;
}

void GroupPolicy::ResetInstance()
{
    s_override = nullptr;
}
#endif

GroupPolicy::GroupPolicy()
{
    const auto result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, c_registryKey, 0, KEY_READ, &m_key);
    if (result == ERROR_PATH_NOT_FOUND || result == ERROR_FILE_NOT_FOUND)
    {
        // Key doesn't exist - all policies not configured.
        return;
    }

    LOG_IF_WIN32_ERROR(result);
}

std::optional<DWORD> GroupPolicy::ReadDwordValue(LPCWSTR valueName) const
{
    if (!m_key)
    {
        return std::nullopt;
    }

    DWORD value = 0;
    DWORD size = sizeof(value);
    const auto result = RegGetValueW(m_key.get(), nullptr, valueName, RRF_RT_REG_DWORD, nullptr, &value, &size);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND)
    {
        return std::nullopt;
    }

    if (result != ERROR_SUCCESS)
    {
        LOG_WIN32_MSG(result, "Error reading policy value: %ls", valueName);
        return std::nullopt;
    }

    return value;
}

PolicyState GroupPolicy::GetState(LPCWSTR policyName) const
{
    auto value = ReadDwordValue(policyName);
    if (!value.has_value())
    {
        return PolicyState::NotConfigured;
    }

    return value.value() != 0 ? PolicyState::Enabled : PolicyState::Disabled;
}

bool GroupPolicy::IsEnabled(LPCWSTR policyName) const
{
    auto state = GetState(policyName);
    return state != PolicyState::Disabled; // NotConfigured -> allowed
}

} // namespace wsl::windows::wslc::settings
