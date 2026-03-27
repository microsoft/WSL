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
    // Use a function-local static to defer path initialization until first use.
    static const std::filesystem::path defaultPath = {wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / "wslc"};

    // TODO: Have a configuration file for those.
    SessionOptions options{};
    options.m_sessionSettings.DisplayName = s_DefaultSessionName;
    options.m_sessionSettings.CpuCount = 4;
    options.m_sessionSettings.MemoryMb = 2048;
    options.m_sessionSettings.BootTimeoutMs = 30 * 1000;
    options.m_sessionSettings.StoragePath = defaultPath.c_str();
    options.m_sessionSettings.MaximumStorageSizeMb = 10000; // 10GB.
    options.m_sessionSettings.NetworkingMode = WSLCNetworkingModeVirtioProxy;
    return options;
}

const WSLCSessionSettings* SessionOptions::Get() const
{
    return &m_sessionSettings;
}
} // namespace wsl::windows::wslc::models
