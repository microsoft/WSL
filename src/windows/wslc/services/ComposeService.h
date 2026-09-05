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

struct ComposeService
{
    static std::vector<models::ComposeProjectInformation> List(models::Session& Session, bool All);
    static void Create(Terminal& Terminal, models::Session& Session, const std::wstring& Path, HANDLE CancelEvent);
    static int Up(Terminal& Terminal, models::Session& Session, const std::wstring& Path, HANDLE CancelEvent);
    static void Start(Terminal& Terminal, models::Session& Session, const std::wstring& Path, HANDLE CancelEvent);
    static int Attach(Terminal& Terminal, models::Session& Session, const std::wstring& Path, HANDLE CancelEvent);
    static void Stop(Terminal& Terminal, models::Session& Session, const std::wstring& Path, ULONG Timeout, HANDLE CancelEvent);
};

} // namespace wsl::windows::wslc::services
