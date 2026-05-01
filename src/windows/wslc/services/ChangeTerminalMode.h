/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ChangeTerminalMode.h

Abstract:

    This file contains the ChangeTerminalMode definition.

--*/
#pragma once

namespace wsl::windows::wslc::services {

class ChangeTerminalMode
{
public:
    NON_COPYABLE(ChangeTerminalMode);
    NON_MOVABLE(ChangeTerminalMode);

    ChangeTerminalMode(HANDLE console, bool cursorVisible) : m_console(console)
    {
        if (!wsl::windows::common::wslutil::IsConsoleHandle(console))
        {
            m_console = nullptr;
            return;
        }

        THROW_IF_WIN32_BOOL_FALSE(GetConsoleCursorInfo(console, &m_originalCursorInfo));
        CONSOLE_CURSOR_INFO newCursorInfo = m_originalCursorInfo;
        newCursorInfo.bVisible = cursorVisible;
        THROW_IF_WIN32_BOOL_FALSE(SetConsoleCursorInfo(console, &newCursorInfo));
    }

    ~ChangeTerminalMode()
    {
        if (m_console)
        {
            LOG_IF_WIN32_BOOL_FALSE(SetConsoleCursorInfo(m_console, &m_originalCursorInfo));
        }
    }

    bool IsConsole() const
    {
        return m_console != nullptr;
    }

private:
    HANDLE m_console{};
    CONSOLE_CURSOR_INFO m_originalCursorInfo{};
};

// RAII helper that enables ENABLE_VIRTUAL_TERMINAL_PROCESSING on a console handle and
// restores the original mode on destruction. No-op if the handle isn't a console or
// VT processing is already enabled.
class EnableVirtualTerminal
{
public:
    NON_COPYABLE(EnableVirtualTerminal);
    NON_MOVABLE(EnableVirtualTerminal);

    explicit EnableVirtualTerminal(HANDLE console)
    {
        DWORD mode;
        if (GetConsoleMode(console, &mode))
        {
            const DWORD newMode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            if (newMode != mode && SetConsoleMode(console, newMode))
            {
                m_console = console;
                m_originalMode = mode;
            }
        }
    }

    ~EnableVirtualTerminal()
    {
        if (m_console)
        {
            LOG_IF_WIN32_BOOL_FALSE(SetConsoleMode(m_console, m_originalMode));
        }
    }

private:
    HANDLE m_console = nullptr;
    DWORD m_originalMode = 0;
};

} // namespace wsl::windows::wslc::services
