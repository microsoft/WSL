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
using namespace wsl::windows::common;

auto ImageProgressCallback::MoveToLine(SHORT line)
{
    if (line > 0)
    {
        wslutil::Print(std::format(L"\033[{}A", line));
    }

    return wil::scope_exit([line = line]() {
        if (line > 1)
        {
            wslutil::Print(std::format(L"\033[{}B", line - 1));
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
            wslutil::PrintMessage(status);
            m_currentLine++;
            return S_OK;
        }

        auto info = Info();

        auto it = m_statuses.find(id);
        if (it == m_statuses.end())
        {
            // If this is the first time we see this ID, create a new line for it.
            m_statuses.emplace(id, m_currentLine);
            wslutil::PrintMessage(GenerateStatusLine(status, id, current, total, info));
            m_currentLine++;
        }
        else
        {
            auto revert = MoveToLine(m_currentLine - it->second);
            wslutil::PrintMessage(GenerateStatusLine(status, id, current, total, info));
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
        line = std::format(L"{} '{}': {}%", status, id, current * 100 / total);
    }
    else if (current != 0)
    {
        line = std::format(L"{} '{}': {}s", status, id, current);
    }
    else
    {
        line = std::format(L"{} '{}'", status, id);
    }

    // Erase any previously written char on that line.
    while (line.size() < info.dwSize.X)
    {
        line += L' ';
    }

    return line;
}
} // namespace wsl::windows::wslc::services
