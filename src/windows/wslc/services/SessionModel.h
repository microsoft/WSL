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

struct SessionOptions
{
    static SessionOptions Default();
    const WSLA_SESSION_SETTINGS* Get() const;

private:
    WSLA_SESSION_SETTINGS m_sessionSettings{};
    inline static std::filesystem::path m_defaultPath = {wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / "wsla"};
};
} // namespace wsl::windows::wslc::models
