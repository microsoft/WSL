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
    static int Attach(const std::wstring& name);
    static wsl::windows::wslc::models::Session CreateSession(const std::optional<wsl::windows::wslc::models::SessionOptions>& options = std::nullopt);
    static std::vector<SessionInformation> List();
};
} // namespace wsl::windows::wslc::services
