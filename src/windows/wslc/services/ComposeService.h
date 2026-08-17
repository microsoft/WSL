/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ComposeService.h

Abstract:

    Defines minimal compose CLI operations.

--*/

#pragma once

#include "SessionModel.h"
#include "Terminal.h"

namespace wsl::windows::wslc::services {

struct ComposeService
{
    static void Create(models::Session& Session, const std::wstring& Path);
    static void Start(models::Session& Session, const std::wstring& Path);
    static int Attach(Terminal& Terminal, models::Session& Session, const std::wstring& Path);
    static void Stop(models::Session& Session, const std::wstring& Path, ULONG Timeout);

private:
    static wil::com_ptr<IWSLCComposeSession> Open(models::Session& Session, const std::wstring& Path);
};

} // namespace wsl::windows::wslc::services
