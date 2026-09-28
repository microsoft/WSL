/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleProgressBar.h

Abstract:

    This file contains the ConsoleProgressBar definition

--*/

#pragma once
#include "windows.h"

namespace wsl::windows::common {
class ConsoleProgressBar
{
public:
    ConsoleProgressBar();

    HRESULT
    Print(_In_ uint64_t progress, _In_ uint64_t total);

    HRESULT
    Clear();

private:
    HRESULT
    PrintAndResetPosition(_In_ LPCWSTR string) const;

    static bool HandleIsConsole(_In_ HANDLE handle);

    bool m_isOutputConsole;
    HANDLE m_outputHandle;
    wil::unique_hlocal_string m_progressString;
    uint64_t m_previousProgress = 0;
    uint64_t m_previousTotal = 0;
};
} // namespace wsl::windows::common
