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

auto ImageProgressCallback::MoveToLine(SHORT line)
{
    if (line > 0)
    {
        wprintf(L"\033[%iA", line);
    }

    return wil::scope_exit([line = line]() {
        if (line > 1)
        {
            wprintf(L"\033[%iB", line - 1);
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
            wprintf(L"%hs\n", status);
            m_currentLine++;
            return S_OK;
        }

        auto info = Info();

        auto it = m_statuses.find(id);
        if (it == m_statuses.end())
        {
            // If this is the first time we see this ID, create a new line for it.
            m_statuses.emplace(id, m_currentLine);
            wprintf(L"%ls\n", GenerateStatusLine(status, id, current, total, info).c_str());
            m_currentLine++;
        }
        else
        {
            auto revert = MoveToLine(m_currentLine - it->second);
            wprintf(L"%ls\n", GenerateStatusLine(status, id, current, total, info).c_str());
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

        std::wstring bar(c_progressBarWidth, L' ');
        for (int i = 0; i < filled; ++i)
        {
            bar[i] = L'=';
        }

        if (filled < c_progressBarWidth)
        {
            bar[filled] = L'>';
        }

        line = std::format(
            L"{}: {} [{}] {}/{}",
            id,
            status,
            bar,
            wsl::shared::string::FormatBytes(current),
            wsl::shared::string::FormatBytes(total));
    }
    else if (current != 0)
    {
        line = std::format(L"{}: {} {}s", id, status, current);
    }
    else
    {
        line = std::format(L"{}: {}", id, status);
    }

    // Truncate to console width to prevent wrapping that would break cursor repositioning.
    if (line.size() > static_cast<size_t>(info.dwSize.X))
    {
        line.resize(info.dwSize.X);
    }

    // Erase any previously written char on that line.
    while (line.size() < info.dwSize.X)
    {
        line += L' ';
    }

    return line;
}
} // namespace wsl::windows::wslc::services
