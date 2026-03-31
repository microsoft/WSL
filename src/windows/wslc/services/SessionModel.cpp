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

const std::filesystem::path& SessionOptions::GetStoragePath()
{
    static const std::filesystem::path storagePath =
        settings::User().Get<settings::Setting::SessionStoragePath>().empty()
            ? std::filesystem::path{wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / SessionOptions::s_defaultStorageSubPath}
            : settings::User().Get<settings::Setting::SessionStoragePath>().c_str();
    return storagePath;
}

SessionOptions::SessionOptions()
{
    m_sessionSettings.DisplayName = s_defaultSessionName;
    m_sessionSettings.StoragePath = GetStoragePath().c_str();
    m_sessionSettings.CpuCount = settings::User().Get<settings::Setting::SessionCpuCount>();
    m_sessionSettings.MemoryMb = settings::User().Get<settings::Setting::SessionMemoryMb>();
    m_sessionSettings.BootTimeoutMs = s_defaultBootTimeoutMs;
    m_sessionSettings.MaximumStorageSizeMb = settings::User().Get<settings::Setting::SessionStorageSizeMb>();
    m_sessionSettings.NetworkingMode = settings::User().Get<settings::Setting::SessionNetworkingMode>();
    m_sessionSettings.FeatureFlags = WslcFeatureFlagsVirtioFs;
}

} // namespace wsl::windows::wslc::models
