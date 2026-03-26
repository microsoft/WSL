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
SessionOptions SessionOptions::Default()
{
    // Use a function-local static to defer path initialization until first use.
    static const std::filesystem::path storagePath =
        settings::User().Get<settings::Setting::SessionStoragePath>().empty()
            ? std::filesystem::path{wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wslc\\defaultstorage"}
            : settings::User().Get<settings::Setting::SessionStoragePath>().c_str();

    SessionOptions options{};
    options.m_sessionSettings.DisplayName = s_DefaultSessionName;
    options.m_sessionSettings.CpuCount = settings::User().Get<settings::Setting::SessionCpuCount>();
    options.m_sessionSettings.MemoryMb = settings::User().Get<settings::Setting::SessionMemoryMb>();
    options.m_sessionSettings.BootTimeoutMs = 30 * 1000;
    options.m_sessionSettings.StoragePath = storagePath.c_str();
    options.m_sessionSettings.MaximumStorageSizeMb = settings::User().Get<settings::Setting::SessionStorageSizeMb>();
    options.m_sessionSettings.NetworkingMode = WSLANetworkingModeVirtioProxy;
    return options;
}

const WSLASessionSettings* SessionOptions::Get() const
{
    return &m_sessionSettings;
}
} // namespace wsl::windows::wslc::models
