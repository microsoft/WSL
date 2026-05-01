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

    // The handle must remain valid for the lifetime of this callback.
    void SetCancelEvent(HANDLE cancelEvent)
    {
        m_cancelEvent = cancelEvent;
    }

private:
    static constexpr SHORT c_maxDisplayLines = 16;
    static constexpr auto c_redrawInterval = std::chrono::milliseconds(50);

    void CollapseWindow();
    void Redraw();
    void WriteTerminal(std::wstring_view content) const;
    bool IsCancelled() const;

    bool m_verbose = false;
    HANDLE m_cancelEvent = nullptr;
    HANDLE m_console = GetStdHandle(STD_OUTPUT_HANDLE);
    bool m_isConsole = wsl::windows::common::wslutil::IsConsoleHandle(m_console);
    EnableVirtualTerminal m_vtMode{m_console};
    std::deque<std::string> m_lines;
    // TODO: Track per step so the destructor can replay only the failing step's logs on
    // error (like docker build). Per-stage tracking could also support separate scrolling
    // windows for parallel stages.
    std::vector<std::string> m_allLines;
    std::string m_pendingLine;
    SHORT m_displayedLines = 0;
    std::chrono::steady_clock::time_point m_lastRedraw{};
    // Captured at construction so the destructor can detect destruction during exception unwinding.
    int m_uncaughtExceptions = std::uncaught_exceptions();
};
} // namespace wsl::windows::wslc::services
