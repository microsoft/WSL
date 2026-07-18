/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionService.h

Abstract:

    This file contains the SessionService definition

--*/
#pragma once

#include "SessionModel.h"
#include "Reporter.h"
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
    static int Attach(Reporter& reporter, const wsl::windows::wslc::models::Session& session);
    static int Enter(Reporter& reporter, const std::wstring& storagePath, const std::wstring& displayName);
    static std::vector<SessionInformation> List();
    // Opens an existing session by name. Throws if not found.
    static wsl::windows::wslc::models::Session OpenSession(const std::wstring& name);
    // Opens the default session. Throws WSLC_E_SESSION_NOT_FOUND if no default session exists.
    static wsl::windows::wslc::models::Session OpenDefaultSession();
    // Opens or creates the default session.
    static wsl::windows::wslc::models::Session OpenOrCreateDefaultSession(Reporter& reporter);
    // Runs the given command and arguments in a session without a TTY, resolving the executable from PATH.
    static int Run(Reporter& reporter, const wsl::windows::wslc::models::Session& session, const std::vector<std::string>& arguments);
    static int TerminateSession(Reporter& reporter, const wsl::windows::wslc::models::Session& session);

private:
    // Common open-only session lookup with unified error handling.
    static wsl::windows::wslc::models::Session OpenSessionByName(const wil::com_ptr<IWSLCSessionManager>& manager, LPCWSTR displayName);
};
} // namespace wsl::windows::wslc::services
