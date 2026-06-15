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
    DWORD written;
    LOG_IF_WIN32_BOOL_FALSE(WriteConsoleW(m_console, content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
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
        if (!m_terminalMode.IsConsole())
        {
            return S_OK;
        }

        if (id == nullptr || *id == '\0') // Print all 'global' statuses on their own line
        {
            WriteTerminal(std::format(L"{}\n", status));
            m_currentLine++;
            return S_OK;
        }

        auto info = Info();

        auto it = m_statuses.find(id);
        if (it == m_statuses.end())
        {
            // If this is the first time we see this ID, create a new line for it.
            m_statuses.emplace(id, m_currentLine);
            WriteTerminal(GenerateStatusLine(status, id, current, total, info) + L'\n');
            m_currentLine++;
        }
        else
        {
            auto revert = MoveToLine(m_currentLine - it->second);
            WriteTerminal(GenerateStatusLine(status, id, current, total, info) + L'\n');
        }

        return S_OK;
    }
    CATCH_RETURN();
}

CONSOLE_SCREEN_BUFFER_INFO ImageProgressCallback::Info()
{
    CONSOLE_SCREEN_BUFFER_INFO info{};
    THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info));
    return info;
}

std::wstring ImageProgressCallback::GenerateStatusLine(LPCSTR status, LPCSTR id, ULONGLONG current, ULONGLONG total, const CONSOLE_SCREEN_BUFFER_INFO& info)
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

    // Use the visible window width (not the buffer width) to prevent wrapping.
    const auto visibleWidth = std::max(0, static_cast<int>(info.srWindow.Right) - info.srWindow.Left + 1);

    // Truncate to console width to prevent wrapping that would break cursor repositioning.
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

    // Erase any previously written char on that line.
    line.resize(visibleWidth, L' ');

    return line;
}
} // namespace wsl::windows::wslc::services
