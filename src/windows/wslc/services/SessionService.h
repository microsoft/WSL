/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionService.h

Abstract:

    This file contains the SessionService definition

--*/
#pragma once

#include "SessionModel.h"
#include <wslc.h>

namespace wsl::windows::wslc::services {
struct SessionInformation
{
    ULONG SessionId;
    DWORD CreatorPid;
    std::wstring DisplayName;
};

struct SessionService
{
    static int Attach(const wsl::windows::wslc::models::Session& session);
    static int Enter(const std::wstring& storagePath, const std::wstring& displayName);
    static std::vector<SessionInformation> List();
    // Opens an existing session by name. Throws if not found.
    static wsl::windows::wslc::models::Session OpenSession(const std::wstring& name);
    // Opens or creates the default session.
    static wsl::windows::wslc::models::Session OpenOrCreateDefaultSession();
    // Runs the given command and arguments in a session without a TTY, resolving the executable from PATH.
    static int Run(const wsl::windows::wslc::models::Session& session, const std::vector<std::string>& arguments);
    static int TerminateSession(const wsl::windows::wslc::models::Session& session);

private:
    // Common session-open logic shared by OpenSession and OpenOrCreateDefaultSession.
    static wsl::windows::wslc::models::Session OpenSessionByName(const wil::com_ptr<IWSLCSessionManager>& manager, const std::wstring& name);
};
} // namespace wsl::windows::wslc::services
