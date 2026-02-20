/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PullImageCallback.h

Abstract:

    This file contains the PullImageCallback definition

--*/
#pragma once
#include "SessionService.h"

namespace wsl::windows::wslc::services {

class ChangeTerminalMode
{
public:
    NON_COPYABLE(ChangeTerminalMode);
    NON_MOVABLE(ChangeTerminalMode);
    ChangeTerminalMode(HANDLE console, bool cursorVisible);
    ~ChangeTerminalMode();

private:
    HANDLE m_console{};
    CONSOLE_CURSOR_INFO m_originalCursorInfo{};
};

class DECLSPEC_UUID("7A1D3376-835A-471A-8DC9-23653D9962D0") PullImageCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback, IFastRundown>
{
public:
    auto MoveToLine(SHORT line);
    HRESULT OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total) override;

private:
    static CONSOLE_SCREEN_BUFFER_INFO Info();
    std::wstring GenerateStatusLine(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total, const CONSOLE_SCREEN_BUFFER_INFO& info);
    std::map<std::string, SHORT> m_statuses;
    SHORT m_currentLine = 0;
    ChangeTerminalMode m_terminalMode{GetStdHandle(STD_OUTPUT_HANDLE), false};
};
} // namespace wsl::windows::wslc::services