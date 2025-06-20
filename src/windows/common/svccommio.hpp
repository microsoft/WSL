/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    svccommio.hpp

Abstract:

    This file contains function declarations for the SvcCommIo helper class.

--*/

#pragma once

#include <wil/filesystem.h>
#include <wil/result.h>
#include "wslservice.h"

typedef struct _LXSS_STD_HANDLES_INFO
{
    HANDLE InputHandle;
    HANDLE OutputHandle;
    HANDLE ErrorHandle;
    wil::unique_hfile ConsoleOutputHandle;
    BOOLEAN IsConsoleInput;
    BOOLEAN IsConsoleOutput;
    BOOLEAN IsConsoleError;
    DWORD SavedInputMode;
    DWORD SavedOutputMode;
    UINT SavedInputCP;
    UINT SavedOutputCP;
} LXSS_STD_HANDLES_INFO, *PLXSS_STD_HANDLES_INFO;

namespace wsl::windows::common {
class SvcCommIo
{
public:
    SvcCommIo();
    ~SvcCommIo();

    PLXSS_STD_HANDLES GetStdHandles();
    COORD GetWindowSize() const;

private:
    void RestoreConsoleMode() const;

    LXSS_STD_HANDLES _stdHandles{};
    LXSS_STD_HANDLES_INFO _stdHandlesInfo{};
};
} // namespace wsl::windows::common
