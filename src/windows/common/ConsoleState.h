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

    // Sets the console output code page to UTF-8 for the lifetime of this object, restoring the
    // saved code page on destruction, without altering any console modes. Use for non-interactive
    // relays that stream raw UTF-8 bytes (e.g. container logs, non-TTY process output) so
    // multi-byte characters render correctly under any console output code page.
    void SetOutputCodePageUtf8();

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
    ULONGLONG m_ownerCreationTime{};
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
