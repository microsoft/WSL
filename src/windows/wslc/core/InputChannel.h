/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InputChannel.h

Abstract:

    Byte source used by Reporter for user input. Reads a line at a time from the
    console (or a redirected file/pipe). For console sources the channel can mask
    echo while reading (password entry) and reports whether input is interactive.

--*/
#pragma once

#include "defs.h"

#include <cstdio>
#include <optional>
#include <string>
#include <Windows.h>

namespace wsl::windows::wslc {

class InputChannel
{
public:
    NON_COPYABLE(InputChannel);
    NON_MOVABLE(InputChannel);

    // Console path: probes the handle for console mode (masking, interactivity);
    // reads are always drawn from readFile (the CRT stdin stream).
    InputChannel(HANDLE consoleHandle, FILE* readFile);

    // FILE* path with explicit interactivity override (for tests). No console
    // handle, so masking is a no-op and interactivity is whatever is passed.
    InputChannel(FILE* readFile, bool interactiveOverride);

    // True when input is attached to a console (a prompt can be shown, echo can be masked).
    bool IsInteractive() const noexcept;

    // Reads a single line, stripping the trailing CR and/or LF. Returns nullopt at
    // end of input with nothing read (so an empty line and EOF are distinguishable).
    // When mask is true and input is an interactive console, console echo is disabled
    // for the duration of the read (restored on return, including on exception).
    std::optional<std::wstring> ReadLine(bool mask) const;

private:
    HANDLE m_consoleHandle = nullptr;
    FILE* m_file = nullptr;
    bool m_interactiveOverride = false;
};

} // namespace wsl::windows::wslc
