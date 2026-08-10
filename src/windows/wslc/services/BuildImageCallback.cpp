/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    BuildImageCallback.cpp

Abstract:

    This file contains the BuildImageCallback Implementation.

--*/

#include "precomp.h"
#include "BuildImageCallback.h"

namespace wsl::windows::wslc::services {

using wsl::windows::common::string::MultiByteToWide;
using namespace wsl::windows::common::vt;

// Fallback width used when the console width can't be queried.
constexpr int c_fallbackConsoleWidth = 79;

BuildImageCallback::~BuildImageCallback()
try
{
    // Capture any partial line so it's included in the error replay below; otherwise
    // CollapseWindow() would discard it.
    if (!m_pendingLine.empty())
    {
        m_pendingLine += '\n';
        m_allLines.push_back(std::move(m_pendingLine));
    }

    CollapseWindow();

    // On build error (not cancellation), replay the full log output so the user can see what went wrong.
    if (!IsCancelled() && std::uncaught_exceptions() > m_uncaughtExceptions && !m_allLines.empty())
    {
        for (const auto& line : m_allLines)
        {
            m_terminal.Info(L"{}", line);
        }
    }
}
CATCH_LOG()

bool BuildImageCallback::IsCancelled() const
{
    return WaitForSingleObject(m_cancelEvent, 0) == WAIT_OBJECT_0;
}

const wsl::windows::common::vt::Sequence& BuildImageCallback::Color(const wsl::windows::common::vt::Sequence& sequence) const
{
    static const wsl::windows::common::vt::Sequence empty{};
    return m_color ? sequence : empty;
}

void BuildImageCallback::CaptureForReplay(std::string_view text)
{
    m_allLines.emplace_back(text);
    m_allLinesBytes += m_allLines.back().size();
    while (m_allLinesBytes > c_maxAllLinesBytes && !m_allLines.empty())
    {
        m_allLinesBytes -= m_allLines.front().size();
        m_allLines.pop_front();
    }
}

void BuildImageCallback::CollapseWindow()
{
    if (m_displayedLines > 0)
    {
        // Move cursor up to the start of the display area, then erase to end of screen.
        m_terminal.Info(L"{}{}", Cursor::Up(m_displayedLines), Erase::ScreenForward);
        m_displayedLines = 0;
    }

    m_lines.clear();
    m_pendingLine.clear();
    m_pullLines.clear();
}

void BuildImageCallback::RedrawIfNeeded()
{
    auto now = std::chrono::steady_clock::now();
    if (now - m_lastRedraw >= c_redrawInterval)
    {
        Redraw();
        m_lastRedraw = now;
    }
}

HRESULT BuildImageCallback::OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total)
try
{
    if (status == nullptr || *status == '\0')
    {
        return S_OK;
    }

    // When cancellation is pending, skip all processing so the server's IO loop can
    // return to its event wait and detect the cancel event promptly.
    if (IsCancelled())
    {
        return S_OK;
    }

    // rawjson: the server forwards docker's raw progress JSON verbatim; print it as-is with no rendering.
    if (m_mode == models::ProgressMode::RawJson)
    {
        m_terminal.Info(L"{}", MultiByteToWide(status));
        return S_OK;
    }

    const std::string_view idView = (id != nullptr) ? id : std::string_view{};
    const bool isLog = (idView == "log");
    const bool isPullProgress = (!idView.empty() && total > 0 && !isLog);

    // quiet: suppress live progress but retain log output so the destructor can replay it on build failure.
    if (m_mode == models::ProgressMode::Quiet)
    {
        if (isLog)
        {
            CaptureForReplay(status);
        }
        return S_OK;
    }

    if (m_verbose || !m_renderInPlace)
    {
        // Only major steps are reported here. Unlike docker's plain output, which appends
        // throttled download lines, pull progress is omitted entirely: without in-place
        // updates those lines are mostly noise.
        if (!isPullProgress)
        {
            m_terminal.Info(L"{}", status);
        }
        return S_OK;
    }

    // Pull/download progress: update the per-entry map so Redraw can show each entry
    // on a single line that updates in place.
    if (isPullProgress)
    {
        m_pullLines[id] = status;
        RedrawIfNeeded();

        return S_OK;
    }

    if (isLog)
    {
        // Log line: add to the scrolling window.
        for (const char* p = status; *p != '\0'; ++p)
        {
            if (*p == '\n')
            {
                // Store with the trailing newline so the byte count matches what is replayed.
                // Cap retained log output to avoid unbounded growth on very long builds.
                m_allLines.push_back(m_pendingLine + '\n');
                m_allLinesBytes += m_allLines.back().size();
                while (m_allLinesBytes > c_maxAllLinesBytes && !m_allLines.empty())
                {
                    m_allLinesBytes -= m_allLines.front().size();
                    m_allLines.pop_front();
                }

                m_lines.push_back(std::move(m_pendingLine));
                m_pendingLine.clear();
                if (m_lines.size() > c_maxDisplayLines)
                {
                    m_lines.pop_front();
                }
            }
            else if (*p == '\r')
            {
                // \r\n is a line ending; standalone \r overwrites the current line.
                if (*(p + 1) != '\n')
                {
                    // Flush a throttled redraw before clearing so \r-based progress
                    // updates are visible even when batched in a single OnProgress call.
                    if (!m_pendingLine.empty())
                    {
                        RedrawIfNeeded();
                    }
                    m_pendingLine.clear();
                }
            }
            else
            {
                m_pendingLine += *p;
            }
        }

        // Throttle redraws to avoid blocking the server's IO loop with console writes
        // during rapid output. Lines accumulate in the deque immediately; the display
        // catches up at ~20fps.
        RedrawIfNeeded();

        return S_OK;
    }

    // Else is a build step
    CollapseWindow();
    auto wide = MultiByteToWide(status);
    const auto bodyLength = wide.find_last_not_of(L"\r\n") + 1;
    const auto newlines = wide.substr(bodyLength);
    wide.resize(bodyLength);

    // Pass the color sequences as arguments (not baked into the string) so Terminal strips
    // them when --no-color is set. Color() additionally strips them outside Tty mode. The
    // trailing newlines are emitted after the reset.
    m_terminal.Info(L"{}{}{}{}", Color(Format::Fg::BrightGreen), wide, Color(Format::Default), newlines);
    return S_OK;
}
CATCH_RETURN();

void BuildImageCallback::Redraw()
{
    const int consoleWidth = m_terminal.GetConsoleWidth(Terminal::Level::Info).value_or(c_fallbackConsoleWidth);

    const bool showPending = !m_pendingLine.empty();
    const int pullCount = static_cast<int>(m_pullLines.size());
    int completedCount = static_cast<int>(m_lines.size());
    const int reservedLines = (showPending ? 1 : 0) + pullCount;
    if (completedCount + reservedLines > c_maxDisplayLines)
    {
        completedCount = std::max(0, c_maxDisplayLines - reservedLines);
    }
    const int displayCount = completedCount + reservedLines;

    // Build the frame body in one buffer to minimize console writes. The cursor moves,
    // erases, and text lines it holds are non-color VT. This only runs in Tty mode, where a
    // VT console is attached. The cursor hide/show wrapper and the dim intensity attribute are
    // passed as Sequence arguments to Terminal (below) so it strips the color ones (Dim/Normal)
    // when --no-color is set, while leaving the non-color cursor moves intact.
    //
    // m_frameBuffer is a member so its backing allocation is reused across frames -
    // it grows to the high-water mark and is never freed between redraws.
    m_frameBuffer.clear();

    // Move cursor to the start of the display area and erase from there to the end of
    // the screen. \033[J handles the case where the new display is shorter than the
    // previous one (e.g. when \r clears the pending line without a replacement).
    if (m_displayedLines > 0)
    {
        m_frameBuffer += Cursor::Up(m_displayedLines);
        m_frameBuffer += Erase::ScreenForward;
    }

    auto appendLine = [&](const std::string& line) {
        auto wline = MultiByteToWide(line);
        if (wline.size() > static_cast<size_t>(consoleWidth))
        {
            wline.resize(static_cast<size_t>(consoleWidth));
        }
        m_frameBuffer += std::move(wline);
        m_frameBuffer += Erase::LineForward;
        m_frameBuffer += L'\n';
    };

    // Print completed lines (skip older ones if we need room for the pending line).
    auto it = m_lines.begin();
    if (completedCount < static_cast<int>(m_lines.size()))
    {
        std::advance(it, m_lines.size() - completedCount);
    }
    for (; it != m_lines.end(); ++it)
    {
        appendLine(*it);
    }

    // Print the in-progress line (e.g. \r-based progress updates).
    if (showPending)
    {
        appendLine(m_pendingLine);
    }

    // Render per-entry pull progress (each entry updates in place via the map).
    for (const auto& [key, line] : m_pullLines)
    {
        appendLine(line);
    }

    // Emit the frame as a single atomic write. Cursor Hide/Show are non-color and always
    // rendered here (VT is on); Format::Dim/Normal are color sequences that Terminal strips
    // under --no-color. The buffered body carries the cursor moves, erases, and text lines.
    m_terminal.Info(L"{}{}{}{}{}", Cursor::Hide, Color(Format::Dim), std::wstring_view{m_frameBuffer}, Color(Format::Normal), Cursor::Show);
    m_displayedLines = displayCount;
}

} // namespace wsl::windows::wslc::services
