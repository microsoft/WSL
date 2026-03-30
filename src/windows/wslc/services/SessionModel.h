/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionModel.h

Abstract:

    This file contains the SessionModel definition

--*/
#pragma once

#include <wslservice.h>
#include <wslc.h>

namespace wsl::windows::wslc::models {

inline constexpr wchar_t s_DefaultSessionName[] = L"wslc-cli";

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

struct SessionOptions
{
    static SessionOptions Default();
    const WSLCSessionSettings* Get() const;

private:
    WSLCSessionSettings m_sessionSettings{};
};
} // namespace wsl::windows::wslc::models
