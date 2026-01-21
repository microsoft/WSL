/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    svccommio.hpp

Abstract:

    This file contains function declarations for the SvcCommIo helper class.

--*/

#pragma once

#include <optional>
#include <wil/filesystem.h>
#include <wil/result.h>
#include "wslservice.h"

namespace wsl::windows::common {

// RAII wrapper for console input configuration and restoration
class ConsoleInput
{
public:
    ConsoleInput(HANDLE Handle, DWORD SavedMode);
    ~ConsoleInput();
    ConsoleInput(const ConsoleInput&) = delete;
    ConsoleInput& operator=(const ConsoleInput&) = delete;
    ConsoleInput(ConsoleInput&&) = delete;
    ConsoleInput& operator=(ConsoleInput&&) = delete;

private:
    HANDLE m_Handle{};
    DWORD m_SavedMode{};
    UINT m_SavedCodePage{};
};

// RAII wrapper for console output configuration and restoration
class ConsoleOutput
{
public:
    ConsoleOutput(wil::unique_hfile&& ConsoleHandle, DWORD SavedMode);
    ~ConsoleOutput();
    ConsoleOutput(const ConsoleOutput&) = delete;
    ConsoleOutput& operator=(const ConsoleOutput&) = delete;
    ConsoleOutput(ConsoleOutput&&) = delete;
    ConsoleOutput& operator=(ConsoleOutput&&) = delete;

private:
    wil::unique_hfile m_ConsoleHandle;
    DWORD m_SavedMode{};
    UINT m_SavedCodePage{};
};

class SvcCommIo
{
public:
    SvcCommIo();

    PLXSS_STD_HANDLES GetStdHandles();
    COORD GetWindowSize() const;

private:
    LXSS_STD_HANDLES m_StdHandles{};
    HANDLE m_WindowSizeHandle = nullptr; // Cached console handle for GetWindowSize

    // RAII members for automatic restoration
    std::optional<ConsoleInput> m_ConsoleInput;
    std::optional<ConsoleOutput> m_ConsoleOutput;
};
} // namespace wsl::windows::common
