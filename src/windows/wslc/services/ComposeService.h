/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeService.h

Abstract:

    Defines minimal compose CLI operations.

--*/

#pragma once

#include "ComposeModel.h"
#include "SessionModel.h"
#include "Terminal.h"

namespace wsl::windows::wslc::services {

using namespace wsl::windows::wslc::cli;

struct ComposeService
{
    static void Create(Terminal& terminal, models::Session& session, const models::ComposeProject& project, HANDLE cancelEvent);
    static int Up(Terminal& terminal, models::Session& session, const models::ComposeProject& project, HANDLE cancelEvent);
    static void Start(Terminal& terminal, models::Session& session, const models::ComposeProject& project, HANDLE cancelEvent);
    static int Attach(Terminal& terminal, models::Session& session, const models::ComposeProject& project, HANDLE cancelEvent);
    static void Stop(Terminal& terminal, models::Session& session, const models::ComposeProject& project, ULONG timeout, HANDLE cancelEvent);
};
} // namespace wsl::windows::wslc::services
