/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    OutputChannel.h

Abstract:

    Byte sink used by Reporter. Each WriteString call is a single WriteConsoleW
    or fwprintf. For console destinations the channel owns VT enablement (RAII).

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

    // Console path: probes handle, enables VT; falls back to fallbackFile when redirected.
    OutputChannel(HANDLE consoleHandle, FILE* fallbackFile);

    // FILE* path with explicit VT override (for tests).
    OutputChannel(FILE* file, bool vtOverride);

    void WriteString(std::wstring_view text) const;

    // Console write width minus one (autowrap guard), or nullopt when redirected.
    std::optional<int> GetConsoleWidth() const;

    bool IsVTEnabled() const noexcept;

private:
    HANDLE m_consoleHandle = nullptr;
    FILE* m_file = nullptr;
    bool m_vtOverride = false;
    std::optional<wsl::windows::common::vt::EnableVirtualTerminal> m_vtMode;
};

} // namespace wsl::windows::wslc
