/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Utils.h

Abstract:

    This file contains the Utils function declarations

--*/
#pragma once

#include "precomp.h"
#include "WSLAProcessLauncher.h"
#include "SessionModel.h"

namespace wsl::windows::wslc::utils {

void PullImpl(wsl::windows::wslc::models::Session& session, const std::string& image);

class ChangeTerminalMode
{
public:
    NON_COPYABLE(ChangeTerminalMode);
    NON_MOVABLE(ChangeTerminalMode);

    ChangeTerminalMode(HANDLE Console, bool CursorVisible) : m_console(Console)
    {
        THROW_IF_WIN32_BOOL_FALSE(GetConsoleCursorInfo(Console, &m_originalCursorInfo));
        CONSOLE_CURSOR_INFO newCursorInfo = m_originalCursorInfo;
        newCursorInfo.bVisible = CursorVisible;

        THROW_IF_WIN32_BOOL_FALSE(SetConsoleCursorInfo(Console, &newCursorInfo));
    }

    ~ChangeTerminalMode()
    {
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleCursorInfo(m_console, &m_originalCursorInfo));
    }

private:
    HANDLE m_console{};
    CONSOLE_CURSOR_INFO m_originalCursorInfo{};
};

} // namespace wsl::windows::wslc::utils