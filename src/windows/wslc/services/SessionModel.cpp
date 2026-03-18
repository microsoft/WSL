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
    namespace s = wsl::windows::wslc::settings;

    // StoragePath is PCWSTR; use a static to own the string for the process lifetime.
    static const std::wstring storagePath = []() -> std::wstring {
        auto configured = s::User().Get<s::Setting::SessionStoragePath>();
        if (!configured.empty())
        {
            return configured;
        }
        return (wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wsla").wstring();
    }();

    SessionOptions options{};
    options.m_sessionSettings.DisplayName          = s_DefaultSessionName;
    options.m_sessionSettings.CpuCount             = s::User().Get<s::Setting::SessionCpuCount>();
    options.m_sessionSettings.MemoryMb             = s::User().Get<s::Setting::SessionMemoryMb>();
    options.m_sessionSettings.BootTimeoutMs        = 30 * 1000;
    options.m_sessionSettings.StoragePath          = storagePath.c_str();
    options.m_sessionSettings.MaximumStorageSizeMb = s::User().Get<s::Setting::SessionStorageSizeMb>();
    options.m_sessionSettings.NetworkingMode       = WSLANetworkingModeVirtioProxy;
    return options;
}

const WSLASessionSettings* SessionOptions::Get() const
{
    return &m_sessionSettings;
}
} // namespace wsl::windows::wslc::models
