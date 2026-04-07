/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionModel.cpp

Abstract:

    This file contains the SessionModel implementation.

--*/

#include <precomp.h>
#include "SessionModel.h"
#include "UserSettings.h"

namespace wsl::windows::wslc::models {

const wchar_t* SessionOptions::GetDefaultSessionName()
{
    return IsElevated() ? s_defaultAdminSessionName : s_defaultSessionName;
}

bool SessionOptions::IsDefaultSessionName(const std::wstring& sessionName)
{
    // Only returns true for the default session name that matches current elevation.
    return wsl::shared::string::IsEqual(sessionName, GetDefaultSessionName());
}

SessionOptions::SessionOptions()
{
    m_sessionSettings.DisplayName = GetDefaultSessionName();
    m_sessionSettings.StoragePath = GetStoragePath().c_str();
    m_sessionSettings.CpuCount = settings::User().Get<settings::Setting::SessionCpuCount>();
    m_sessionSettings.MemoryMb = settings::User().Get<settings::Setting::SessionMemoryMb>();
    m_sessionSettings.BootTimeoutMs = s_defaultBootTimeoutMs;
    m_sessionSettings.MaximumStorageSizeMb = settings::User().Get<settings::Setting::SessionStorageSizeMb>();
    m_sessionSettings.NetworkingMode = settings::User().Get<settings::Setting::SessionNetworkingMode>();
    if (settings::User().Get<settings::Setting::SessionHostFileShareMode>() == settings::HostFileShareMode::VirtioFs)
    {
        WI_SetFlag(m_sessionSettings.FeatureFlags, WslcFeatureFlagsVirtioFs);
    }

    if (settings::User().Get<settings::Setting::SessionDnsTunneling>())
    {
        WI_SetFlag(m_sessionSettings.FeatureFlags, WslcFeatureFlagsDnsTunneling);
    }
}

bool SessionOptions::IsElevated()
{
    auto token = wil::open_current_access_token(TOKEN_QUERY);

    // IsTokenElevated checks if the integrity level is exactly HIGH.
    // We must also check for local system because it is above HIGH.
    // However, IsTokenLocalSystem() does not work correctly and fails.
    // TODO: Add proper handling for system user callers.
    return wsl::windows::common::security::IsTokenElevated(token.get());
}

const std::filesystem::path& SessionOptions::GetStoragePath()
{
    static const std::filesystem::path basePath = []() {
        return settings::User().Get<settings::Setting::SessionStoragePath>().empty()
                   ? std::filesystem::path{wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / SessionOptions::s_defaultStorageSubPath}
                   : settings::User().Get<settings::Setting::SessionStoragePath>().c_str();
    }();

    static const std::filesystem::path storagePathNonAdmin = basePath / std::wstring{s_defaultSessionName};
    static const std::filesystem::path storagePathAdmin = basePath / std::wstring{s_defaultAdminSessionName};

    return IsElevated() ? storagePathAdmin : storagePathNonAdmin;
}

} // namespace wsl::windows::wslc::models
