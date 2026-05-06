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
    // The cancel event handle must remain valid for the lifetime of this callback.
    BuildImageCallback(HANDLE cancelEvent, bool verbose) : m_verbose(verbose), m_cancelEvent(cancelEvent)
    {
    }
    ~BuildImageCallback();
    HRESULT OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total) override;

private:
    static constexpr SHORT c_maxDisplayLines = 16;
    static constexpr auto c_redrawInterval = std::chrono::milliseconds(50);
    static constexpr size_t c_maxAllLinesBytes = 10 * 1024 * 1024; // 10 MiB cap on retained log output for error replay.

    void CollapseWindow();
    void Redraw();
    // Use WriteConsoleW directly rather than wprintf: wprintf is noticeably slower for
    // the per-redraw scrolling display and produces visible flicker.
    void WriteTerminal(std::wstring_view content) const;
    bool IsCancelled() const;

    const bool m_verbose;
    const HANDLE m_cancelEvent;
    HANDLE m_console = GetStdHandle(STD_OUTPUT_HANDLE);
    bool m_isConsole = wsl::windows::common::wslutil::IsConsoleHandle(m_console);
    EnableVirtualTerminal m_vtMode{m_console};
    std::deque<std::string> m_lines;
    // Each entry already contains the trailing newline so the bytes match what's replayed.
    // TODO: Track logs per step so the destructor can replay only the failing step's
    // logs on error, rather than every line captured during the build.
    std::deque<std::string> m_allLines;
    size_t m_allLinesBytes = 0;
    std::string m_pendingLine;
    SHORT m_displayedLines = 0;
    std::chrono::steady_clock::time_point m_lastRedraw{};
    // Captured at construction so the destructor can detect destruction during exception unwinding.
    int m_uncaughtExceptions = std::uncaught_exceptions();
};
} // namespace wsl::windows::wslc::services
