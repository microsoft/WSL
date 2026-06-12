/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageProgressCallback.h

Abstract:

    This file contains the ImageProgressCallback definition

--*/
#pragma once
#include "SessionService.h"
#include "VTSupport.h"
#include <map>
#include <string>

namespace wsl::windows::wslc::services {

// TODO: Handle terminal resizes.
class DECLSPEC_UUID("7A1D3376-835A-471A-8DC9-23653D9962D0") ImageProgressCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback, IFastRundown>
{
public:
    HRESULT OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total) override;

private:
    auto MoveToLine(int line);
    static CONSOLE_SCREEN_BUFFER_INFO Info();
    void WriteTerminal(std::wstring_view content) const;
    std::wstring GenerateStatusLine(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total, const CONSOLE_SCREEN_BUFFER_INFO& info);
    std::map<std::string, int> m_statuses;
    int m_currentLine = 0;
    HANDLE m_console = GetStdHandle(STD_OUTPUT_HANDLE);
    wsl::windows::common::vt::EnableVirtualTerminal m_vtMode{m_console};
    wsl::windows::common::vt::ChangeTerminalMode m_terminalMode{m_console, false};
};
} // namespace wsl::windows::wslc::services
