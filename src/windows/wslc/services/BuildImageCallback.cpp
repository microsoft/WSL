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
            WriteTerminal(MultiByteToWide(line));
        }
    }
}
CATCH_LOG()

void BuildImageCallback::WriteTerminal(std::wstring_view content) const
{
    DWORD written;
    LOG_IF_WIN32_BOOL_FALSE(WriteConsoleW(m_console, content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
}

bool BuildImageCallback::IsCancelled() const
{
    return WaitForSingleObject(m_cancelEvent, 0) == WAIT_OBJECT_0;
}

void BuildImageCallback::CollapseWindow()
{
    if (m_displayedLines > 0)
    {
        WriteTerminal(std::format(L"\033[{}A\033[J", m_displayedLines));
        m_displayedLines = 0;
    }

    m_lines.clear();
    m_pendingLine.clear();
}

HRESULT BuildImageCallback::OnProgress(LPCSTR status, LPCSTR id, ULONGLONG /*current*/, ULONGLONG /*total*/)
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

    if (m_verbose || !m_isConsole)
    {
        wprintf(L"%hs", status);
        return S_OK;
    }

    // Match the specific "log" sentinel sent by WSLCSession::BuildImage rather than
    // accepting any non-empty id, so future or unrelated id usage defaults to permanent.
    const bool isLog = (id != nullptr && std::string_view{id} == "log");

    if (!isLog)
    {
        // Permanent line: collapse the scrolling window then print directly.
        CollapseWindow();
        WriteTerminal(MultiByteToWide(status));
        return S_OK;
    }

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
                auto now = std::chrono::steady_clock::now();
                if (!m_pendingLine.empty() && now - m_lastRedraw >= c_redrawInterval)
                {
                    Redraw();
                    m_lastRedraw = now;
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
    auto now = std::chrono::steady_clock::now();
    if (now - m_lastRedraw >= c_redrawInterval)
    {
        Redraw();
        m_lastRedraw = now;
    }

    return S_OK;
}
CATCH_RETURN();

void BuildImageCallback::Redraw()
{
    CONSOLE_SCREEN_BUFFER_INFO info{};
    THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfo(m_console, &info));
    // Use the visible window width (not buffer width), minus one column to avoid the
    // deferred-wrap edge case when a line is exactly the window width. Clamp to at
    // least zero so the value never goes negative (which would underflow when passed
    // to std::wstring::resize).
    const SHORT consoleWidth = std::max<SHORT>(0, info.srWindow.Right - info.srWindow.Left);

    // Determine how many completed lines to show, leaving room for the pending line.
    const bool showPending = !m_pendingLine.empty();
    SHORT completedCount = static_cast<SHORT>(m_lines.size());
    if (showPending && completedCount >= c_maxDisplayLines)
    {
        completedCount = c_maxDisplayLines - 1;
    }
    const SHORT displayCount = completedCount + (showPending ? 1 : 0);

    // Build the entire frame in one buffer to minimize console writes. Hide the cursor
    // during the redraw so the user doesn't see it bouncing through the cursor movement,
    // then show it again at the final position. The dim attribute (\033[2m) renders the
    // scrolling lines de-emphasized regardless of the user's theme.
    std::wstring buffer = L"\033[?25l\033[2m";

    // Move cursor to the start of the display area and erase from there to the end of
    // the screen. \033[J handles the case where the new display is shorter than the
    // previous one (e.g. when \r clears the pending line without a replacement).
    if (m_displayedLines > 0)
    {
        buffer += std::format(L"\033[{}A\033[J", m_displayedLines);
    }

    auto appendLine = [&](const std::string& line) {
        auto wline = MultiByteToWide(line);
        if (static_cast<SHORT>(wline.size()) > consoleWidth)
        {
            wline.resize(consoleWidth);
        }
        buffer += wline;
        buffer += L"\033[K\n";
    };

    // Print completed lines (skip older ones if we need room for the pending line).
    auto it = m_lines.begin();
    if (completedCount < static_cast<SHORT>(m_lines.size()))
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

    buffer += L"\033[22m\033[?25h";

    WriteTerminal(buffer);
    m_displayedLines = displayCount;
}

} // namespace wsl::windows::wslc::services
