/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ConsoleProgressBar.cpp

Abstract:

    This file contains the ConsoleProgressBar implementation

--*/

#include "precomp.h"
#include "ConsoleProgressBar.h"

constexpr size_t c_progressBarWidth = 58;
constexpr size_t c_lineWidth = c_progressBarWidth + 2;
constexpr size_t c_progressBarBufferSize = c_lineWidth + 1;
constexpr LPCWSTR c_progressBarFormatString = L"[%58s]";
constexpr LPCWSTR c_clearFormatString = L"%60s";

wsl::windows::common::ConsoleProgressBar::ConsoleProgressBar()
{
    m_outputHandle = GetStdHandle(STD_ERROR_HANDLE);
    m_isOutputConsole = HandleIsConsole(m_outputHandle);
    m_progressString = wil::make_hlocal_string_nothrow(nullptr, c_lineWidth);
    THROW_IF_NULL_ALLOC(m_progressString);
}

// Print
//
// formats and prints the progress bar to the console with the given progress indicated
HRESULT
wsl::windows::common::ConsoleProgressBar::Print(_In_ uint64_t progress, _In_ uint64_t total)
{
    if (!m_isOutputConsole)
    {
        return S_FALSE;
    }

    total = std::max<uint64_t>(total, 1);
    progress = std::min<uint64_t>(progress, total);
    if ((progress == m_previousProgress) && (total == m_previousTotal))
    {
        return S_OK;
    }

    const float percent = progress / static_cast<float>(total);
    const size_t filledBarCount = static_cast<size_t>(c_progressBarWidth * percent);
    RETURN_IF_FAILED(StringCchPrintfW(m_progressString.get(), c_progressBarBufferSize, c_progressBarFormatString, L""));

    wil::unique_hlocal_string percentString;
    RETURN_IF_FAILED(wil::str_printf_nothrow(percentString, L"%2.1f%%", percent * 100.0f));

    RETURN_HR_IF(E_INVALIDARG, (wcslen(percentString.get()) > wcslen(m_progressString.get())));

    // Write the 'filled' progress symbol '=' into the bar
    for (size_t i = 0; i < filledBarCount; i++)
    {
        m_progressString.get()[i + 1] = L'=';
    }

    // Insert the percentage-formatted string into the middle of the progress bar
    size_t percentOffset = (c_lineWidth - wcslen(percentString.get())) / 2;
    for (LPWSTR currentPercentCharacter = percentString.get(); L'\0' != *currentPercentCharacter; currentPercentCharacter++)
    {
        m_progressString.get()[percentOffset++] = *currentPercentCharacter;
    }

    RETURN_IF_FAILED(PrintAndResetPosition(m_progressString.get()));

    m_previousProgress = progress;
    m_previousTotal = total;
    return S_OK;
}

// Clear
//
// removes the progress bar from the console
HRESULT
wsl::windows::common::ConsoleProgressBar::Clear()
{
    if (!m_isOutputConsole)
    {
        return S_FALSE;
    }

    RETURN_IF_FAILED(StringCchPrintfW(m_progressString.get(), c_progressBarBufferSize, c_clearFormatString, L""));
    return PrintAndResetPosition(m_progressString.get());
}

// PrintAndResetPosition
//
// This function writes a given wide character string to the given output handle
// and moves the cursor back to the start position
HRESULT
wsl::windows::common::ConsoleProgressBar::PrintAndResetPosition(_In_ LPCWSTR string) const
{
    RETURN_HR_IF_NULL(E_INVALIDARG, string);

    CONSOLE_SCREEN_BUFFER_INFO consoleBufferInfo;
    RETURN_LAST_ERROR_IF(!GetConsoleScreenBufferInfo(m_outputHandle, &consoleBufferInfo));

    DWORD stringLength = static_cast<DWORD>(wcslen(string));
    RETURN_LAST_ERROR_IF(!WriteConsoleW(m_outputHandle, string, stringLength, &stringLength, nullptr));

    RETURN_LAST_ERROR_IF(!SetConsoleCursorPosition(m_outputHandle, consoleBufferInfo.dwCursorPosition));
    return S_OK;
}

// HandleIsConsole
//
// Determines if the given handle is a console
bool wsl::windows::common::ConsoleProgressBar::HandleIsConsole(_In_ HANDLE handle)
{
    DWORD mode;
    return (GetFileType(handle) == FILE_TYPE_CHAR) && GetConsoleMode(handle, &mode);
}
