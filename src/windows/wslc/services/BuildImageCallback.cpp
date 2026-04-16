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
{
    CollapseWindow();

    // On build error (not cancellation), replay the full log output so the user can see what went wrong.
    bool cancelled = (m_cancelEvent != nullptr && WaitForSingleObject(m_cancelEvent, 0) == WAIT_OBJECT_0);
    if (m_terminalMode && !cancelled && std::uncaught_exceptions() > m_uncaughtExceptions && !m_allLines.empty())
    {
        for (const auto& line : m_allLines)
        {
            wprintf(L"%hs\n", line.c_str());
        }
    }
}

void BuildImageCallback::CollapseWindow()
{
    if (m_terminalMode && m_displayedLines > 0)
    {
        wprintf(L"\033[%dA\033[J", m_displayedLines);
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
    if (m_cancelEvent != nullptr && WaitForSingleObject(m_cancelEvent, 0) == WAIT_OBJECT_0)
    {
        return S_OK;
    }

    if (m_verbose || !wsl::windows::common::wslutil::IsConsoleHandle(GetStdHandle(STD_OUTPUT_HANDLE)))
    {
        wprintf(L"%hs", status);
        return S_OK;
    }

    const bool isLog = (id != nullptr && *id != '\0');

    if (!isLog)
    {
        // Permanent line: collapse the scrolling window then print directly.
        CollapseWindow();
        wprintf(L"%hs", status);
        return S_OK;
    }

    // Initialize terminal mode on first log line (hides cursor for scrolling).
    if (!m_terminalMode)
    {
        m_terminalMode.emplace(GetStdHandle(STD_OUTPUT_HANDLE), false);
    }

    // Log line: add to the scrolling window.
    for (const char* p = status; *p != '\0'; ++p)
    {
        if (*p == '\n')
        {
            m_allLines.push_back(m_pendingLine);
            m_lines.push_back(std::move(m_pendingLine));
            m_pendingLine.clear();
            while (m_lines.size() > c_maxDisplayLines)
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
                if (!m_pendingLine.empty() && now - m_lastRedraw >= std::chrono::milliseconds(50))
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
    if (now - m_lastRedraw >= std::chrono::milliseconds(50))
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
    THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info));
    const SHORT consoleWidth = info.dwSize.X;

    // Determine how many completed lines to show, leaving room for the pending line.
    const bool showPending = !m_pendingLine.empty();
    SHORT completedCount = static_cast<SHORT>(m_lines.size());
    if (showPending && completedCount >= c_maxDisplayLines)
    {
        completedCount = c_maxDisplayLines - 1;
    }
    const SHORT displayCount = completedCount + (showPending ? 1 : 0);

    // Build the entire frame in one buffer to minimize console writes.
    std::wstring buffer;

    // Move cursor to the start of the display area.
    if (m_displayedLines > 0)
    {
        buffer += std::format(L"\033[{}A", m_displayedLines);
    }

    // Print completed lines (skip older ones if we need room for the pending line).
    auto it = m_lines.begin();
    if (completedCount < static_cast<SHORT>(m_lines.size()))
    {
        std::advance(it, m_lines.size() - completedCount);
    }
    for (; it != m_lines.end(); ++it)
    {
        auto wline = MultiByteToWide(*it);
        if (static_cast<SHORT>(wline.size()) > consoleWidth)
        {
            wline.resize(consoleWidth);
        }
        buffer += wline;
        buffer += L"\033[K\n";
    }

    // Print the in-progress line (e.g. \r-based progress updates).
    if (showPending)
    {
        auto wline = MultiByteToWide(m_pendingLine);
        if (static_cast<SHORT>(wline.size()) > consoleWidth)
        {
            wline.resize(consoleWidth);
        }
        buffer += wline;
        buffer += L"\033[K\n";
    }

    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    WriteConsoleW(console, buffer.data(), static_cast<DWORD>(buffer.size()), &written, nullptr);
    m_displayedLines = displayCount;
}

} // namespace wsl::windows::wslc::services
