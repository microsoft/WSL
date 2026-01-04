/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    svccommio.cpp

Abstract:

    This file contains function definitions for the SvcCommIo helper class.

--*/

#include "precomp.h"
#include "svccomm.hpp"
#include "svccommio.hpp"
#pragma hdrstop

namespace {

bool IsConsoleHandle(_In_ HANDLE Handle)
{
    DWORD Mode;
    return GetFileType(Handle) == FILE_TYPE_CHAR && GetConsoleMode(Handle, &Mode);
}

void ChangeConsoleMode(_In_ HANDLE Handle, _In_ DWORD Mode)
{
    //
    // Use the invalid parameter error code to detect the v1 console that does
    // not support the provided mode. This can be improved in the future when
    // a more elegant solution exists.
    //
    // N.B. Ignore failures setting the mode if the console has already
    //      disconnected.
    //

    if (!SetConsoleMode(Handle, Mode))
    {
        // DISABLE_NEWLINE_AUTO_RETURN is not supported everywhere, if the flag was present fall back and try again.
        if (WI_IsFlagSet(Mode, DISABLE_NEWLINE_AUTO_RETURN))
        {
            if (SetConsoleMode(Handle, WI_ClearFlag(Mode, DISABLE_NEWLINE_AUTO_RETURN)))
            {
                return;
            }
        }

        switch (GetLastError())
        {
        case ERROR_INVALID_PARAMETER:
            THROW_HR(WSL_E_CONSOLE);

        case ERROR_PIPE_NOT_CONNECTED:
            break;

        default:
            THROW_LAST_ERROR();
        }
    }
}

void TrySetConsoleMode(_In_ HANDLE Handle, _In_ DWORD Mode)
try
{
    ChangeConsoleMode(Handle, Mode);
}
CATCH_LOG()

} // namespace

namespace wsl::windows::common {

std::optional<ConsoleInput> ConsoleInput::Create(HANDLE Handle)
{
    DWORD Mode;
    if (GetFileType(Handle) == FILE_TYPE_CHAR && GetConsoleMode(Handle, &Mode))
    {
        return ConsoleInput(Handle, Mode);
    }

    return std::nullopt;
}

ConsoleInput::ConsoleInput(HANDLE Handle, DWORD SavedMode) : m_Handle(Handle), m_SavedMode(SavedMode)
{
    // Save code page.
    m_SavedCodePage = GetConsoleCP();

    // Configure for raw input with VT support.
    DWORD NewMode = m_SavedMode;
    WI_SetAllFlags(NewMode, ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
    WI_ClearAllFlags(NewMode, ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    ChangeConsoleMode(Handle, NewMode);

    // Set UTF-8 code page.
    LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(CP_UTF8));
}

ConsoleInput::~ConsoleInput()
{
    TrySetConsoleMode(m_Handle, m_SavedMode);
    LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(m_SavedCodePage));
}

std::optional<ConsoleOutput> ConsoleOutput::Create()
{
    wil::unique_hfile ConsoleHandle(
        CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));

    if (ConsoleHandle)
    {
        DWORD Mode;
        if (GetConsoleMode(ConsoleHandle.get(), &Mode))
        {
            return ConsoleOutput(std::move(ConsoleHandle), Mode);
        }
    }

    return std::nullopt;
}

ConsoleOutput::ConsoleOutput(wil::unique_hfile ConsoleHandle, DWORD SavedMode) :
    m_ConsoleHandle(std::move(ConsoleHandle)), m_SavedMode(SavedMode)
{
    // Save code page.
    m_SavedCodePage = GetConsoleOutputCP();

    // Configure for VT output.
    DWORD NewMode = m_SavedMode;
    WI_SetAllFlags(NewMode, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
    ChangeConsoleMode(m_ConsoleHandle.get(), NewMode);

    // Set UTF-8 code page.
    LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(CP_UTF8));
}

ConsoleOutput::~ConsoleOutput()
{
    TrySetConsoleMode(m_ConsoleHandle.get(), m_SavedMode);
    LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(m_SavedCodePage));
}

SvcCommIo::SvcCommIo()
{
    const HANDLE InputHandle = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE OutputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE ErrorHandle = GetStdHandle(STD_ERROR_HANDLE);

    // Configure input console
    m_ConsoleInput = ConsoleInput::Create(InputHandle);

    // Configure output console
    m_ConsoleOutput = ConsoleOutput::Create();

    // Initialize the standard handles structure
    const bool IsConsoleInput = m_ConsoleInput.has_value();
    m_StdHandles.StdIn.HandleType = IsConsoleInput ? LxssHandleConsole : LxssHandleInput;
    m_StdHandles.StdIn.Handle = IsConsoleInput ? LXSS_HANDLE_USE_CONSOLE : HandleToUlong(InputHandle);

    const bool IsConsoleOutput = IsConsoleHandle(OutputHandle);
    m_StdHandles.StdOut.HandleType = IsConsoleOutput ? LxssHandleConsole : LxssHandleOutput;
    m_StdHandles.StdOut.Handle = IsConsoleOutput ? LXSS_HANDLE_USE_CONSOLE : HandleToUlong(OutputHandle);

    const bool IsConsoleError = IsConsoleHandle(ErrorHandle);
    m_StdHandles.StdErr.HandleType = IsConsoleError ? LxssHandleConsole : LxssHandleOutput;
    m_StdHandles.StdErr.Handle = IsConsoleError ? LXSS_HANDLE_USE_CONSOLE : HandleToUlong(ErrorHandle);

    // Cache a console handle for GetWindowSize
    m_WindowSizeHandle = IsConsoleOutput ? OutputHandle : (IsConsoleError ? ErrorHandle : nullptr);
}

PLXSS_STD_HANDLES
SvcCommIo::GetStdHandles()
{
    return &m_StdHandles;
}

COORD
SvcCommIo::GetWindowSize() const
{
    if (m_WindowSizeHandle)
    {
        CONSOLE_SCREEN_BUFFER_INFOEX Info{};
        Info.cbSize = sizeof(Info);
        THROW_IF_WIN32_BOOL_FALSE(GetConsoleScreenBufferInfoEx(m_WindowSizeHandle, &Info));
        return {
            static_cast<short>(Info.srWindow.Right - Info.srWindow.Left + 1),
            static_cast<short>(Info.srWindow.Bottom - Info.srWindow.Top + 1)};
    }

    return {80, 24}; // Default size if no console
}

} // namespace wsl::windows::common
