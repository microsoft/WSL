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

// Controls when ConsoleState attempts to restore the original console state.
enum class RestorePolicy
{
    // Exclusive restore to the original state captured by this instance.
    Exclusive,

    // Restore only when the state still matches what this instance configured.
    Cooperative,
};

// RAII wrapper for console state configuration and restoration
class ConsoleState
{
public:
    explicit ConsoleState(RestorePolicy Policy = RestorePolicy::Exclusive);
    ~ConsoleState();
    ConsoleState(const ConsoleState&) = delete;
    ConsoleState& operator=(const ConsoleState&) = delete;
    ConsoleState(ConsoleState&&) = delete;
    ConsoleState& operator=(ConsoleState&&) = delete;

    COORD GetWindowSize() const;
    void SetInteractiveMode();

private:
    bool AcquireCoordination();
    void ConfigureInteractiveMode();
    void ReleaseCoordination();
    void RestoreConsoleState();

    wil::unique_hfile m_InputHandle;
    wil::unique_hfile m_OutputHandle;
    wil::unique_handle m_coordinationMutex;
    wil::unique_handle m_coordinationMapping;
    void* m_coordinationView{};
    RestorePolicy m_restorePolicy;
    bool m_interactiveModeConfigured{false};
    std::optional<DWORD> m_SavedInputMode{};
    std::optional<DWORD> m_ConfiguredInputMode{};
    std::optional<UINT> m_SavedInputCodePage{};
    std::optional<UINT> m_ConfiguredInputCodePage{};
    std::optional<DWORD> m_SavedOutputMode{};
    std::optional<DWORD> m_ConfiguredOutputMode{};
    std::optional<UINT> m_SavedOutputCodePage{};
    std::optional<UINT> m_ConfiguredOutputCodePage{};
};
} // namespace wsl::windows::common
