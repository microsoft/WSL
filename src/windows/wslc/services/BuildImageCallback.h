/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    BuildImageCallback.h

Abstract:

    This file contains the BuildImageCallback definition

--*/
#pragma once
#include "BuildView.h"
#include "ChangeTerminalMode.h"
#include "SessionService.h"
#include <mutex>
#include <thread>

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
    static constexpr size_t c_maxLogPeekLines = 5;

    std::vector<std::wstring> BuildFrameLines(SHORT consoleWidth) const;
    void RenderFrame();
    void StartTimerThread();
    void StopTimerThread();
    std::wstring FormatStepLine(const ViewStep& step, SHORT consoleWidth) const;
    void WriteTerminal(std::wstring_view content) const;
    bool IsCancelled() const;

    const bool m_verbose;
    const HANDLE m_cancelEvent;
    HANDLE m_console = GetStdHandle(STD_OUTPUT_HANDLE);
    bool m_isConsole = wsl::windows::common::wslutil::IsConsoleHandle(m_console);
    EnableVirtualTerminal m_vtMode{m_console};

    std::mutex m_mutex;
    BuildView m_buildView;

    // Timer thread for periodic redraws
    std::thread m_timerThread;
    wil::unique_event m_stopEvent;

    SHORT m_linesWrittenLastFrame = 0; // Number of lines rendered in the previous frame.
    SHORT m_viewportHeight = 0;        // Terminal height snapshot taken on first render.
    std::chrono::steady_clock::time_point m_buildStartTime = std::chrono::steady_clock::now();

    // Captured at construction so the destructor can detect destruction during exception unwinding.
    int m_uncaughtExceptions = std::uncaught_exceptions();
};
} // namespace wsl::windows::wslc::services
