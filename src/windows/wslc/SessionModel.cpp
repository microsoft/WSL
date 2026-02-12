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
    auto dataFolder = std::filesystem::path(wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr)) / "wsla";
    SessionOptions options{};
    options.m_sessionSettings.DisplayName = L"wsla-cli";
    options.m_sessionSettings.CpuCount = 4;
    options.m_sessionSettings.MemoryMb = 2048;
    options.m_sessionSettings.BootTimeoutMs = 30 * 1000;
    options.StoragePath(std::move(dataFolder));
    options.m_sessionSettings.MaximumStorageSizeMb = 10000; // 10GB.
    options.m_sessionSettings.NetworkingMode = WSLANetworkingModeNAT;
    return options;
}

void SessionOptions::StoragePath(const std::filesystem::path& path)
{
    m_storagePath = path;
    m_sessionSettings.StoragePath = m_storagePath.c_str();
}

SessionOptions::operator const WSLA_SESSION_SETTINGS*() const
{
    return &m_sessionSettings;
}
} // namespace wsl::windows::wslc::models