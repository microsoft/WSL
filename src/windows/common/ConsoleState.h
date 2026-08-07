/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleState.h

Abstract:

    This file contains function declarations for the ConsoleState helper class.

--*/

#pragma once

#include <wil/filesystem.h>
#include <wil/result.h>
#include "wslservice.h"

namespace wsl::windows::common {

// RAII wrapper for console state configuration and restoration
class ConsoleState
{
public:
    ConsoleState();
    ~ConsoleState();
    ConsoleState(const ConsoleState&) = delete;
    ConsoleState& operator=(const ConsoleState&) = delete;
    ConsoleState(ConsoleState&&) = delete;
    ConsoleState& operator=(ConsoleState&&) = delete;

    COORD GetWindowSize() const;
    void SetInteractiveMode();

    // Sets the console output code page to UTF-8 for the lifetime of this object, restoring the
    // saved code page on destruction, without altering any console modes. Use for non-interactive
    // relays that stream raw UTF-8 bytes (e.g. container logs, non-TTY process output) so
    // multi-byte characters render correctly under any console output code page.
    void SetOutputCodePageUtf8();

private:
    void RestoreConsoleState();

    wil::unique_hfile m_InputHandle;
    wil::unique_hfile m_OutputHandle;
    bool m_interactiveModeConfigured{false};
    std::optional<DWORD> m_SavedInputMode{};
    std::optional<UINT> m_SavedInputCodePage{};
    std::optional<DWORD> m_SavedOutputMode{};
    std::optional<UINT> m_SavedOutputCodePage{};
};
} // namespace wsl::windows::common
