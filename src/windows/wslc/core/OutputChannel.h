/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    OutputChannel.h

Abstract:

    OutputChannel is the byte sink used by Reporter.

    Each WriteString call is exactly one underlying WriteConsoleW (when the
    handle is a console) or one fwprintf + one optional fflush (when the
    destination is a FILE*). Both paths are atomic at the kernel/CRT layer, so
    concurrent threads cannot interleave bytes mid-call.

    For console destinations the channel also owns the VT enablement on that
    handle (RAII; original console mode is restored on destruction).

--*/
#pragma once

#include "VTSupport.h"
#include "defs.h"

#include <cstdio>
#include <optional>
#include <string_view>
#include <Windows.h>

namespace wsl::windows::wslc {

class OutputChannel
{
public:
    NON_COPYABLE(OutputChannel);
    NON_MOVABLE(OutputChannel);

    // No destination; WriteString is a no-op.
    OutputChannel() = default;

    // Console-handle path: enables VT processing on the handle (restored on
    // destruction); falls back to fwprintf on fallbackFile when the handle
    // isn't a console (e.g. redirected to a file or pipe).
    OutputChannel(HANDLE consoleHandle, FILE* fallbackFile);

    // FILE*-only path with explicit VT (test pipes; non-console FILE* streams).
    OutputChannel(FILE* file, bool vtEnabled);

    // Single atomic write of text. No-op when text is empty or when there is
    // no destination.
    void WriteString(std::wstring_view text) const;

    // Safe write width of the underlying console (one column reserved as an
    // autowrap guard), or nullopt when the destination is a non-console.
    std::optional<int> GetConsoleWidth() const;

    bool IsVTEnabled() const noexcept
    {
        return m_vtEnabled;
    }

private:
    HANDLE m_consoleHandle = nullptr; // non-null only when the destination is a console
    FILE* m_file = nullptr;           // used when m_consoleHandle is null
    bool m_vtEnabled = false;
    std::optional<wsl::windows::common::vt::EnableVirtualTerminal> m_vtMode; // engaged only on the console path
};

} // namespace wsl::windows::wslc
