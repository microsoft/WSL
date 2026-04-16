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

} // namespace wsl::windows::wslc::services
