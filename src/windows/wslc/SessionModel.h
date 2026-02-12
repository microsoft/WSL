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

    void StoragePath(std::filesystem::path path);

    operator const WSLA_SESSION_SETTINGS*() const;

private:
    WSLA_SESSION_SETTINGS m_sessionSettings;
    std::filesystem::path m_storagePath;
};
} // namespace wsl::windows::wslc::models
