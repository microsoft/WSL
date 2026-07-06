/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ImageProgressCallback.cpp

Abstract:

    This file contains the ImageProgressCallback Implementation.

--*/

#include "precomp.h"
#include "ImageProgressCallback.h"
#include "ImageService.h"
#include <format>

namespace wsl::windows::wslc::services {
using namespace wsl::shared;
using namespace wsl::windows::common::vt;

void ImageProgressCallback::WriteTerminal(std::wstring_view content) const
{
    // Route progress rendering through the Reporter's Info channel (stderr) so it
    // respects the global output state and keeps stdout clean for scripting. Each
    // call is emitted as a single atomic write, matching the previous WriteConsoleW.
    m_reporter.Write(Reporter::Level::Info, L"{}", content);
}

auto ImageProgressCallback::MoveToLine(int line)
{
    if (line > 0)
    {
        WriteTerminal(Cursor::Up(line).Get());
    }

    return wil::scope_exit([line = line, this]() {
        if (line > 1)
        {
            WriteTerminal(Cursor::Down(line - 1).Get());
        }
    });
}

HRESULT ImageProgressCallback::OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total)
{
    try
    {
        // The in-place progress display relies on cursor movement, so it only renders
        // when the Info channel is an interactive VT console. When redirected, skip it.
        if (!m_vtEnabled)
        {
            return S_OK;
        }

        // Hide the cursor while rendering so the user doesn't see it bouncing through the
        // cursor movements, then restore it at the final position. Uses VT sequences to match
        // BuildImageCallback. scope_exit guarantees the cursor is shown again on every path.
        WriteTerminal(Cursor::Hide.Get());
        auto showCursor = wil::scope_exit([this]() { WriteTerminal(Cursor::Show.Get()); });

        if (id == nullptr || *id == '\0') // Print all 'global' statuses on their own line
        {
            WriteTerminal(std::format(L"{}\n", status));
            m_currentLine++;
            return S_OK;
        }

        const auto visibleWidth = m_reporter.GetConsoleWidth(Reporter::Level::Info);

        auto it = m_statuses.find(id);
        if (it == m_statuses.end())
        {
            // If this is the first time we see this ID, create a new line for it.
            m_statuses.emplace(id, m_currentLine);
            WriteTerminal(GenerateStatusLine(status, id, current, total, visibleWidth) + L'\n');
            m_currentLine++;
        }
        else
        {
            auto revert = MoveToLine(m_currentLine - it->second);
            WriteTerminal(GenerateStatusLine(status, id, current, total, visibleWidth) + L'\n');
        }

        return S_OK;
    }
    CATCH_RETURN();
}

std::wstring ImageProgressCallback::GenerateStatusLine(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total, std::optional<int> visibleWidth)
{
    std::wstring line;
    if (total != 0)
    {
        constexpr int c_progressBarWidth = 30;

        int filled = 0;
        if (current >= total)
        {
            filled = c_progressBarWidth;
        }
        else
        {
            auto ratio = static_cast<long double>(current) / static_cast<long double>(total);
            filled = static_cast<int>(ratio * c_progressBarWidth);
        }

        filled = std::clamp(filled, 0, c_progressBarWidth);

        std::wstring bar;
        bar.reserve(c_progressBarWidth);
        bar.append(filled, L'=');
        bar.append(L">");
        bar.resize(c_progressBarWidth, L' ');

        // Docker's reported total is an estimate of the compressed layer size, so the actual bytes
        // transferred can exceed it. Drop the total in that case to avoid displaying a count over 100%.
        auto progress = wsl::shared::string::FormatBytes(current);

        if (current <= total)
        {
            progress += std::format(L"/{}", wsl::shared::string::FormatBytes(total));
        }

        line = std::format(L"{}: {} [{}] {}", id, status, bar, progress);
    }
    else if (current != 0)
    {
        line = std::format(L"{}: {} {}", id, status, wsl::shared::string::FormatBytes(current));
    }
    else
    {
        line = std::format(L"{}: {}", id, status);
    }

    // Truncate to the console width to prevent wrapping that would break cursor repositioning.
    // When the width is unknown (redirected) the line is written as-is.
    if (visibleWidth)
    {
        if (line.size() > static_cast<size_t>(*visibleWidth))
        {
            line.resize(*visibleWidth);

            // Avoid splitting a surrogate pair — if the last code unit is a high surrogate,
            // drop it so we don't emit an invalid UTF-16 sequence.
            if (!line.empty() && IS_HIGH_SURROGATE(line.back()))
            {
                line.pop_back();
            }
        }

        // Erase any previously written char on that line.
        line.resize(*visibleWidth, L' ');
    }

    return line;
}
} // namespace wsl::windows::wslc::services
