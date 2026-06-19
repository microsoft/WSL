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

    const std::optional<std::wstring>& DisplayName() const noexcept
    {
        return m_displayName;
    }

    void SetDisplayName(std::wstring name)
    {
        m_displayName = std::move(name);
    }

private:
    wil::com_ptr<IWSLCSession> m_session;
    std::optional<std::wstring> m_displayName;
};

} // namespace wsl::windows::wslc::models