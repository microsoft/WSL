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

} // namespace wsl::windows::wslc::models