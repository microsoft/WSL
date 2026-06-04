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
    auto MoveToLine(int line);
    HRESULT OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total) override;

private:
    static CONSOLE_SCREEN_BUFFER_INFO Info();
    std::wstring GenerateStatusLine(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total, const CONSOLE_SCREEN_BUFFER_INFO& info);
    std::map<std::string, int> m_statuses;
    int m_currentLine = 0;
    wsl::windows::common::vt::EnableVirtualTerminal m_vtMode{GetStdHandle(STD_OUTPUT_HANDLE)};
    wsl::windows::common::vt::ChangeTerminalMode m_terminalMode{GetStdHandle(STD_OUTPUT_HANDLE), false};
};
} // namespace wsl::windows::wslc::services
