/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SessionService.h

Abstract:

    This file contains the SessionService definition

--*/
#pragma once

#include "Reporter.h"
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
    static int Attach(Reporter& output, const std::wstring& name);
    // Creates a default session with server-determined name and settings.
    static wsl::windows::wslc::models::Session CreateDefaultSession();
    static int Enter(Reporter& output, const std::wstring& storagePath, const std::wstring& displayName);
    static std::vector<SessionInformation> List();
    static wsl::windows::wslc::models::Session OpenSession(const std::wstring& displayName);
    static int TerminateSession(Reporter& output, const std::wstring& displayName);
};
} // namespace wsl::windows::wslc::services
