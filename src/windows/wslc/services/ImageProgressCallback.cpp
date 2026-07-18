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

// Fallback width for the in-place progress display when the console width can't be queried. This
// value already includes the autowrap guard (visible width minus one) so a wrapped line can't
// corrupt the cursor-based rendering.
constexpr int c_fallbackConsoleWidth = 79;

auto ImageProgressCallback::MoveToLine(int line)
{
    if (line > 0)
    {
        m_reporter.Write(m_level, L"{}", Cursor::Up(line));
    }

    // scope_exit is noexcept and may fire during unwinding; scope_exit_log swallows output
    // failures so a throw here can't call std::terminate.
    return wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [line = line, this]() {
        if (line > 1)
        {
            m_reporter.Write(m_level, L"{}", Cursor::Down(line - 1));
        }
    });
}

HRESULT ImageProgressCallback::OnProgress(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total)
{
    try
    {
        // status is [unique] in the IDL, so it may be null; normalize before either path uses it.
        status = (status != nullptr) ? status : "";

        // The in-place progress display needs cursor movement, so when output is redirected fall
        // back to a log stream: one line per new status, deduping the repeated byte-progress
        // callbacks that share a status text.
        if (!m_vtEnabled)
        {
            if (id == nullptr || *id == '\0')
            {
                m_reporter.Write(m_level, L"{}\n", status);
            }
            else
            {
                auto [it, inserted] = m_lastStatusById.try_emplace(id, status);
                if (inserted || it->second != status)
                {
                    it->second = status;
                    m_reporter.Write(m_level, L"{}: {}\n", id, status);
                }
            }

            return S_OK;
        }

        // Hide the cursor while rendering so it doesn't bounce through the movements; scope_exit_log
        // restores it on every exit path and can't call std::terminate during unwinding.
        m_reporter.Write(m_level, L"{}", Cursor::Hide);
        auto showCursor = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [this]() { m_reporter.Write(m_level, L"{}", Cursor::Show); });

        if (id == nullptr || *id == '\0') // Print all 'global' statuses on their own line
        {
            m_reporter.Write(m_level, L"{}\n", status);
            m_currentLine++;
            return S_OK;
        }

        const int visibleWidth = m_reporter.GetConsoleWidth(m_level).value_or(c_fallbackConsoleWidth);

        auto it = m_statuses.find(id);
        if (it == m_statuses.end())
        {
            // If this is the first time we see this ID, create a new line for it.
            m_statuses.emplace(id, m_currentLine);
            m_reporter.Write(m_level, L"{}\n", GenerateStatusLine(status, id, current, total, visibleWidth));
            m_currentLine++;
        }
        else
        {
            auto revert = MoveToLine(m_currentLine - it->second);
            m_reporter.Write(m_level, L"{}\n", GenerateStatusLine(status, id, current, total, visibleWidth));
        }

        return S_OK;
    }
    CATCH_RETURN();
}

std::wstring ImageProgressCallback::GenerateStatusLine(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total, int visibleWidth)
{
    // status/id are [unique] in the IDL and may be null; treat null as empty before formatting.
    const char* const safeStatus = (status != nullptr) ? status : "";
    const char* const safeId = (id != nullptr) ? id : "";

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

        line = std::format(L"{}: {} [{}] {}", safeId, safeStatus, bar, progress);
    }
    else if (current != 0)
    {
        line = std::format(L"{}: {} {}", safeId, safeStatus, wsl::shared::string::FormatBytes(current));
    }
    else
    {
        line = std::format(L"{}: {}", safeId, safeStatus);
    }

    // Truncate to the console width to prevent wrapping that breaks cursor repositioning, then pad
    // to erase any previously written characters on the line.
    if (line.size() > static_cast<size_t>(visibleWidth))
    {
        line.resize(visibleWidth);

        // Avoid splitting a surrogate pair — if the last code unit is a high surrogate,
        // drop it so we don't emit an invalid UTF-16 sequence.
        if (!line.empty() && IS_HIGH_SURROGATE(line.back()))
        {
            line.pop_back();
        }
    }

    line.resize(visibleWidth, L' ');

    return line;
}
} // namespace wsl::windows::wslc::services
