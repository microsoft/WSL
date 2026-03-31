/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionModel.h

Abstract:

    This file contains the SessionModel definition

--*/
#pragma once

#include <wslc.h>

namespace wsl::windows::wslc::models {

struct Session
{
    explicit Session(wil::com_ptr<IWSLCSession> session) : m_session(std::move(session))
    {
    }
    IWSLCSession* Get() const noexcept
    {
        return m_session.get();
    }

private:
    wil::com_ptr<IWSLCSession> m_session;
};

class SessionOptions
{
public:
    static constexpr const wchar_t s_defaultSessionName[] = L"wslc-cli";
    static constexpr const wchar_t s_defaultStorageSubPath[] = L"wslc\\storage";
    static constexpr uint32_t s_defaultBootTimeoutMs = 30 * 1000;

    static SessionOptions Default()
    {
        return SessionOptions();
    }

    SessionOptions();

    const WSLCSessionSettings* Get() const
    {
        return &m_sessionSettings;
    }

private:
    static const std::filesystem::path& GetStoragePath();
    WSLCSessionSettings m_sessionSettings{};
};

} // namespace wsl::windows::wslc::models