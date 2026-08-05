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
    NON_COPYABLE(Session);
    DEFAULT_MOVABLE(Session);

    explicit Session(wil::com_ptr<IWSLCSession> session) : m_session(std::move(session))
    {
    }

    IWSLCSession* Get() const noexcept
    {
        return m_session.get();
    }

    // Acquires an activity token that keeps the VM alive for the duration of a client-side
    // container operation (resolve + operate, plus any streamed output). Hold the returned
    // pointer for the whole operation; releasing it lets the VM idle-terminate again.
    [[nodiscard]] wil::com_ptr<IUnknown> BeginContainerOperation() const
    {
        wil::com_ptr<IUnknown> operation;
        THROW_IF_FAILED(m_session->BeginContainerOperation(&operation));
        return operation;
    }

private:
    wil::com_ptr<IWSLCSession> m_session;
};

} // namespace wsl::windows::wslc::models