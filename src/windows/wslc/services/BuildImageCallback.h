/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    BuildImageCallback.h

Abstract:

    This file contains the BuildImageCallback definition

--*/
#pragma once
#include "ChangeTerminalMode.h"
#include "SessionService.h"
#include <deque>

namespace wsl::windows::wslc::services {
class DECLSPEC_UUID("3EDD5DBF-CA6C-4CF7-923A-AD94B6A732E5") BuildImageCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback, IFastRundown>
{
public:
    ~BuildImageCallback();
    HRESULT OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total) override;

    void SetVerbose(bool verbose)
    {
        m_verbose = verbose;
    }

    void SetCancelEvent(HANDLE cancelEvent)
    {
        m_cancelEvent = cancelEvent;
    }

private:
    static constexpr SHORT c_maxDisplayLines = 16;

    void CollapseWindow();
    void Redraw();

    bool m_verbose = false;
    HANDLE m_cancelEvent = nullptr;
    std::deque<std::string> m_lines;
    std::vector<std::string> m_allLines;
    std::string m_pendingLine;
    SHORT m_displayedLines = 0;
    std::chrono::steady_clock::time_point m_lastRedraw{};
    int m_uncaughtExceptions = std::uncaught_exceptions();
    std::optional<ChangeTerminalMode> m_terminalMode;
};
} // namespace wsl::windows::wslc::services
