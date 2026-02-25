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
    using namespace wsl::windows::wslc::settings;

    UserSettings settings;

    SessionOptions options{};
    options.m_sessionSettings.DisplayName = L"wsla-cli";
    options.m_sessionSettings.CpuCount = settings.Get<Setting::CpuCount>();
    options.m_sessionSettings.MemoryMb = settings.Get<Setting::MemoryMb>();
    options.m_sessionSettings.BootTimeoutMs = settings.Get<Setting::BootTimeoutMs>();
    options.m_sessionSettings.MaximumStorageSizeMb = settings.Get<Setting::MaximumStorageSizeMb>();

    // Store path in member so the LPCWSTR pointer remains valid.
    options.m_storagePath = settings.Get<Setting::StoragePath>();
    options.m_sessionSettings.StoragePath = options.m_storagePath.c_str();

    auto networkingMode = settings.Get<Setting::NetworkingMode>();
    options.m_sessionSettings.NetworkingMode =
        (networkingMode == SessionNetworkingMode::Nat) ? WSLANetworkingModeNAT : WSLANetworkingModeNone;

    return options;
}

const WSLA_SESSION_SETTINGS* SessionOptions::Get()
{
    // Re-assign pointer in case the object was moved.
    m_sessionSettings.StoragePath = m_storagePath.c_str();
    return &m_sessionSettings;
}
} // namespace wsl::windows::wslc::models
