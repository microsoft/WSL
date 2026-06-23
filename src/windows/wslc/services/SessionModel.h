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

    explicit Session(wil::com_ptr<IWSLCSession> session, wil::com_ptr<IWarningCallback> warningCallback = {}) :
        m_session(std::move(session)), m_warningCallback(std::move(warningCallback))
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

    // Kept alive for the lifetime of the session model (i.e. the whole CLI command) so the service
    // can deliver warnings emitted by lazy/background work — such as resource recovery on the first
    // VM start — back to this CLI invocation, even though no single COM call carries the callback.
    wil::com_ptr<IWarningCallback> m_warningCallback;
};

} // namespace wsl::windows::wslc::models