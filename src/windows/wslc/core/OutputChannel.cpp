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

namespace wsl::windows::wslc {

OutputChannel::OutputChannel(HANDLE consoleHandle, FILE* fallbackFile)
{
    DWORD mode = 0;
    if (consoleHandle != INVALID_HANDLE_VALUE && consoleHandle != nullptr && GetConsoleMode(consoleHandle, &mode))
    {
        m_consoleHandle = consoleHandle;
        m_vtMode.emplace(consoleHandle);
        m_vtEnabled = m_vtMode->IsVTEnabled();
    }
    else
    {
        WI_ASSERT(fallbackFile != nullptr);
        m_file = fallbackFile;
    }
}

OutputChannel::OutputChannel(FILE* file, bool vtEnabled) : m_file(file), m_vtEnabled(vtEnabled)
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

    if (m_file == nullptr)
    {
        return;
    }

    if (fwprintf(m_file, L"%.*ls", static_cast<int>(text.size()), text.data()) < 0)
    {
        const int err = errno;
        LOG_HR_MSG(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), "fwprintf to redirected output failed (errno=%d)", err);
    }

    // Windows CRT defaults stderr to _IONBF, so skip the no-op fflush for it.
    if (m_file != stderr && fflush(m_file) != 0)
    {
        const int err = errno;
        LOG_HR_MSG(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), "fflush of redirected output failed (errno=%d)", err);
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

    // Visible width is (Right - Left + 1); reserve one column as an autowrap
    // guard so callers can write the full returned width without risking an
    // unwanted line break.
    return std::max(0, static_cast<int>(info.srWindow.Right) - static_cast<int>(info.srWindow.Left));
}

} // namespace wsl::windows::wslc
