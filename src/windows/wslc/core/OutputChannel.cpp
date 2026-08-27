/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    OutputChannel.cpp

Abstract:

    Implementation of OutputChannel.

--*/
#include "precomp.h"
#include "OutputChannel.h"

#include <algorithm>
#include <cerrno>

namespace wsl::windows::wslc {

OutputChannel::OutputChannel(HANDLE consoleHandle, FILE* fallbackFile)
{
    DWORD mode = 0;
    if (consoleHandle != INVALID_HANDLE_VALUE && consoleHandle != nullptr && GetConsoleMode(consoleHandle, &mode))
    {
        m_consoleHandle = consoleHandle;
        m_vtMode.emplace(consoleHandle);
    }
    else
    {
        WI_ASSERT(fallbackFile != nullptr);
        m_file = fallbackFile;
    }
}

OutputChannel::OutputChannel(FILE* file, bool vtOverride) : m_file(file), m_vtOverride(vtOverride)
{
    WI_ASSERT(file != nullptr);
}

void OutputChannel::WriteString(std::wstring_view text) const
{
    if (text.empty())
    {
        return;
    }

    if (m_consoleHandle != nullptr)
    {
        DWORD written = 0;
        LOG_IF_WIN32_BOOL_FALSE(WriteConsoleW(m_consoleHandle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr));
        return;
    }

    if (fwprintf(m_file, L"%.*ls", static_cast<int>(text.size()), text.data()) < 0)
    {
        const int err = errno;
        LOG_HR_MSG(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), "fwprintf to redirected output failed (errno=%d)", err);
    }
}

std::optional<int> OutputChannel::GetConsoleWidth() const
{
    if (m_consoleHandle == nullptr)
    {
        return std::nullopt;
    }

    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(m_consoleHandle, &info))
    {
        return std::nullopt;
    }

    // (Right - Left + 1) is the visible width; subtract one more as an autowrap guard.
    return std::max(0, static_cast<int>(info.srWindow.Right) - static_cast<int>(info.srWindow.Left));
}

bool OutputChannel::IsVTEnabled() const noexcept
{
    if (m_consoleHandle != nullptr)
    {
        DWORD mode = 0;
        return GetConsoleMode(m_consoleHandle, &mode) && WI_IsFlagSet(mode, ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    return m_vtOverride;
}

} // namespace wsl::windows::wslc
