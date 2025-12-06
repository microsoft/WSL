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

namespace wsl::windows::common {

// RAII wrapper for console input configuration and restoration
class ConsoleInput
{
public:
    static std::unique_ptr<ConsoleInput> Create(HANDLE Handle);
    ~ConsoleInput();
    ConsoleInput(const ConsoleInput&) = delete;
    ConsoleInput& operator=(const ConsoleInput&) = delete;

private:
    ConsoleInput(HANDLE Handle, DWORD SavedMode);

    HANDLE m_Handle = nullptr;
    DWORD m_SavedMode = 0;
    UINT m_SavedCodePage = 0;
};

// RAII wrapper for console output configuration and restoration
class ConsoleOutput
{
public:
    static std::unique_ptr<ConsoleOutput> Create();
    ~ConsoleOutput();
    ConsoleOutput(const ConsoleOutput&) = delete;
    ConsoleOutput& operator=(const ConsoleOutput&) = delete;

private:
    ConsoleOutput(wil::unique_hfile ConsoleHandle, DWORD SavedMode);

    wil::unique_hfile m_ConsoleHandle;
    DWORD m_SavedMode = 0;
    UINT m_SavedCodePage = 0;
};

class SvcCommIo
{
public:
    SvcCommIo();
    ~SvcCommIo() = default;

    PLXSS_STD_HANDLES GetStdHandles();
    COORD GetWindowSize() const;

private:
    LXSS_STD_HANDLES m_StdHandles{};
    HANDLE m_WindowSizeHandle = nullptr; // Cached console handle for GetWindowSize

    // RAII members for automatic restoration
    std::unique_ptr<ConsoleInput> m_ConsoleInput;
    std::unique_ptr<ConsoleOutput> m_ConsoleOutput;
};
} // namespace wsl::windows::common
