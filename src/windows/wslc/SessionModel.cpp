/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionModel.cpp

Abstract:

    This file contains the SessionModel implementation.

--*/

#include <precomp.h>
#include "SessionModel.h"

namespace wsl::windows::wslc::models {
SessionOptions SessionOptions::Default()
{
    // TODO: Have a configuration file for those.
    SessionOptions options{};
    options.m_sessionSettings.DisplayName = L"wsla-cli";
    options.m_sessionSettings.CpuCount = 4;
    options.m_sessionSettings.MemoryMb = 2048;
    options.m_sessionSettings.BootTimeoutMs = 30 * 1000;
    options.m_sessionSettings.StoragePath = m_defaultPath.c_str();
    options.m_sessionSettings.MaximumStorageSizeMb = 10000; // 10GB.
    options.m_sessionSettings.NetworkingMode = WSLANetworkingModeNAT;
    return options;
}

const WSLA_SESSION_SETTINGS* SessionOptions::Get() const
{
    return &m_sessionSettings;
}
} // namespace wsl::windows::wslc::models