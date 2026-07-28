/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    BuildImageCallback.h

Abstract:

    This file contains the BuildImageCallback definition

--*/
#pragma once
#include "ContainerModel.h"
#include "Terminal.h"
#include "SessionService.h"
#include "VTSupport.h"
#include <deque>
#include <map>

namespace wsl::windows::wslc::services {
class DECLSPEC_UUID("3EDD5DBF-CA6C-4CF7-923A-AD94B6A732E5") BuildImageCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IProgressCallback, IFastRundown>
{
public:
    // The cancel event handle must remain valid for the lifetime of this callback.
    // Mode selects the rendering style (Auto is expected to already be resolved to Tty/Plain by the caller).
    BuildImageCallback(Terminal& terminal, HANDLE cancelEvent, bool verbose, models::ProgressMode mode = models::ProgressMode::Tty) :
        m_terminal(terminal), m_verbose(verbose), m_cancelEvent(cancelEvent), m_mode(mode), m_color(mode == models::ProgressMode::Tty)
    {
    }
    ~BuildImageCallback();
    HRESULT OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total) override;

private:
    static constexpr int c_maxDisplayLines = 16;
    static constexpr auto c_redrawInterval = std::chrono::milliseconds(50);
    static constexpr size_t c_maxAllLinesBytes = 10 * 1024 * 1024; // 10 MiB cap on retained log output for error replay.

    void CollapseWindow();
    void Redraw();
    void RedrawIfNeeded();
    bool IsCancelled() const;
    // Appends a log chunk to the error-replay buffer, enforcing the retained-bytes cap.
    void CaptureForReplay(std::string_view text);
    // Returns the sequence when color is enabled for this callback, else an empty (no-op) sequence so
    // the Terminal emits nothing for it. Used to strip color in plain mode while keeping cursor moves.
    const wsl::windows::common::vt::Sequence& Color(const wsl::windows::common::vt::Sequence& sequence) const;

    Terminal& m_terminal;
    const bool m_verbose;
    const HANDLE m_cancelEvent;
    const models::ProgressMode m_mode;
    const bool m_color;
    bool m_isConsole = m_terminal.IsVTEnabled(Terminal::Level::Info);
    std::deque<std::string> m_lines;
    // Each entry already contains the trailing newline so the bytes match what's replayed.
    // TODO: Track logs per step so the destructor can replay only the failing step's
    // logs on error, rather than every line captured during the build.
    std::deque<std::string> m_allLines;
    size_t m_allLinesBytes = 0;
    std::string m_pendingLine;
    int m_displayedLines = 0;
    std::chrono::steady_clock::time_point m_lastRedraw{};
    // Per-entry pull progress lines, keyed by entry id. Updated in place by Redraw. std::map so order is consistent.
    std::map<std::string, std::string> m_pullLines;
    // Reused across Redraw() calls so the backing allocation grows to the high-water
    // mark and is then reused rather than re-allocated every frame.
    std::wstring m_frameBuffer;
    // Captured at construction so the destructor can detect destruction during exception unwinding.
    int m_uncaughtExceptions = std::uncaught_exceptions();
};
} // namespace wsl::windows::wslc::services
