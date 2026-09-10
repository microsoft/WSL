/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InputChannel.cpp

Abstract:

    Implementation of InputChannel.

--*/
#include "precomp.h"
#include "InputChannel.h"

#include <wil/resource.h>

namespace wsl::windows::wslc {

InputChannel::InputChannel(HANDLE consoleHandle, FILE* readFile) : m_file(readFile)
{
    DWORD mode = 0;
    if (consoleHandle != INVALID_HANDLE_VALUE && consoleHandle != nullptr && GetConsoleMode(consoleHandle, &mode))
    {
        m_consoleHandle = consoleHandle;
    }
}

InputChannel::InputChannel(FILE* readFile, bool interactiveOverride) :
    m_file(readFile), m_interactiveOverride(interactiveOverride)
{
}

bool InputChannel::IsInteractive() const noexcept
{
    if (m_consoleHandle != nullptr)
    {
        DWORD mode = 0;
        return GetConsoleMode(m_consoleHandle, &mode) != FALSE;
    }

    return m_interactiveOverride;
}

std::optional<std::wstring> InputChannel::ReadLine(bool mask) const
{
    if (m_file == nullptr)
    {
        return std::nullopt;
    }

    // Disable console echo while reading when masking is requested and input is a
    // real console. Armed only after echo is actually disabled so the restore is a
    // no-op otherwise; runs on every exit path including exceptions.
    DWORD previousMode = 0;
    bool echoDisabled = false;
    auto restoreEcho = wil::scope_exit([&]() {
        if (echoDisabled)
        {
            LOG_IF_WIN32_BOOL_FALSE(SetConsoleMode(m_consoleHandle, previousMode));
        }
    });

    if (mask && m_consoleHandle != nullptr && GetConsoleMode(m_consoleHandle, &previousMode))
    {
        // Fail rather than echo a secret if the mode cannot be changed.
        THROW_IF_WIN32_BOOL_FALSE(SetConsoleMode(m_consoleHandle, previousMode & ~ENABLE_ECHO_INPUT));
        echoDisabled = true;
    }

    std::wstring line;
    bool anyRead = false;
    for (;;)
    {
        const wint_t ch = fgetwc(m_file);
        if (ch == WEOF)
        {
            break;
        }

        anyRead = true;
        if (ch == L'\n')
        {
            break;
        }

        line.push_back(static_cast<wchar_t>(ch));
    }

    if (!anyRead)
    {
        return std::nullopt;
    }

    // The read stops at LF; strip a paired CR so callers get a bare line regardless
    // of the input's line-ending convention.
    if (!line.empty() && line.back() == L'\r')
    {
        line.pop_back();
    }

    return line;
}

} // namespace wsl::windows::wslc
