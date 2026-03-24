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
    std::wstring GetStoragePath()
    {
        // Storage path is determined once at runtime and remains static thereafter.
        static const std::wstring storagePath =
            (wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / SessionOptions::s_defaultSessionName).wstring();
        return storagePath;
    }
} // namespace

SessionOptions::SessionOptions(uint32_t cpuCount, uint32_t memoryMb, uint32_t bootTimeoutMs, uint64_t maximumStorageSizeMb, WSLANetworkingMode networkingMode) :
    m_storagePath(GetStoragePath())
{
    m_sessionSettings.DisplayName = s_defaultSessionName;
    m_sessionSettings.StoragePath = m_storagePath.c_str();
    m_sessionSettings.CpuCount = cpuCount;
    m_sessionSettings.MemoryMb = memoryMb;
    m_sessionSettings.BootTimeoutMs = bootTimeoutMs;
    m_sessionSettings.MaximumStorageSizeMb = maximumStorageSizeMb;
    m_sessionSettings.NetworkingMode = networkingMode;
}
} // namespace wsl::windows::wslc::models
