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

private:
    void RestoreConsoleState();

    wil::unique_hfile m_InputHandle;
    wil::unique_hfile m_OutputHandle;
    std::optional<DWORD> m_SavedInputMode{};
    std::optional<UINT> m_SavedInputCodePage{};
    std::optional<DWORD> m_SavedOutputMode{};
    std::optional<UINT> m_SavedOutputCodePage{};
};
} // namespace wsl::windows::common
