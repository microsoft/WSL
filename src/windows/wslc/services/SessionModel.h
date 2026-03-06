/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionModel.h

Abstract:

    This file contains the SessionModel definition

--*/
#pragma once

#include <wslservice.h>
#include <wslaservice.h>

namespace wsl::windows::wslc::models {

struct Session
{
    explicit Session(wil::com_ptr<IWSLASession> session) : m_session(std::move(session))
    {
    }
    IWSLASession* Get() const noexcept
    {
        return m_session.get();
    }

private:
    wil::com_ptr<IWSLASession> m_session;
};

class SessionOptions
{
public:
    // Default values as constexpr
    static constexpr const wchar_t c_defaultDisplayName[] = L"wslc-cli";
    static constexpr const wchar_t c_storagePathPrefix[] = L"wslc";
    static constexpr uint32_t c_defaultCpuCount = 4;
    static constexpr uint32_t c_defaultMemoryMb = 2048;
    static constexpr uint32_t c_defaultBootTimeoutMs = 30000;
    static constexpr uint64_t c_defaultMaximumStorageSizeMb = 10000; // 10GB
    static constexpr WSLANetworkingMode c_defaultNetworkingMode = WSLANetworkingModeVirtioProxy;

    static SessionOptions Default();

    explicit SessionOptions(
        std::wstring displayName = c_defaultDisplayName,
        uint32_t cpuCount = c_defaultCpuCount,
        uint32_t memoryMb = c_defaultMemoryMb,
        uint32_t bootTimeoutMs = c_defaultBootTimeoutMs,
        uint64_t maximumStorageSizeMb = c_defaultMaximumStorageSizeMb,
        WSLANetworkingMode networkingMode = c_defaultNetworkingMode);

    void SetDisplayName(const std::wstring& displayName);

    const WSLA_SESSION_SETTINGS* Get() const
    {
        return &m_sessionSettings;
    }

private:
    std::wstring m_displayName;
    std::wstring m_storagePath;
    WSLA_SESSION_SETTINGS m_sessionSettings{};
};
} // namespace wsl::windows::wslc::models