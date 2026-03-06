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

namespace {
    std::wstring GetStoragePath(const std::wstring& displayName)
    {
        // To avoid collisions with other sessions, storage path uses display name as part of its path.
        const std::filesystem::path storagePath =
            wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / SessionOptions::c_storagePathPrefix / displayName;
        return storagePath.wstring();
    }
} // namespace

SessionOptions::SessionOptions(std::wstring displayName, uint32_t cpuCount, uint32_t memoryMb, uint32_t bootTimeoutMs, uint64_t maximumStorageSizeMb, WSLANetworkingMode networkingMode) :
    m_displayName(std::move(displayName)), m_storagePath(GetStoragePath(m_displayName))
{
    m_sessionSettings.DisplayName = m_displayName.c_str();
    m_sessionSettings.StoragePath = m_storagePath.c_str();
    m_sessionSettings.CpuCount = cpuCount;
    m_sessionSettings.MemoryMb = memoryMb;
    m_sessionSettings.BootTimeoutMs = bootTimeoutMs;
    m_sessionSettings.MaximumStorageSizeMb = maximumStorageSizeMb;
    m_sessionSettings.NetworkingMode = networkingMode;
}

SessionOptions SessionOptions::Default()
{
    return SessionOptions();
}

void SessionOptions::SetDisplayName(const std::wstring& displayName)
{
    // Setting the display name also affects the storage path.
    m_displayName = displayName;
    m_storagePath = GetStoragePath(m_displayName);
    m_sessionSettings.DisplayName = m_displayName.c_str();
    m_sessionSettings.StoragePath = m_storagePath.c_str();
}

} // namespace wsl::windows::wslc::models
