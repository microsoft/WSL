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

void TrySetConsoleMode(_In_ HANDLE Handle, _In_ DWORD Mode)
{
    if (!SetConsoleMode(Handle, Mode))
    {
        const auto Error = GetLastError();
        if (Error != ERROR_INVALID_PARAMETER && Error != ERROR_PIPE_NOT_CONNECTED)
        {
            LOG_IF_WIN32_ERROR(Error);
        }
    }
}

} // namespace

namespace wsl::windows::common {

// ConsoleInput implementation
std::unique_ptr<ConsoleInput> ConsoleInput::Create(HANDLE Handle)
{
    DWORD Mode;
    if (GetFileType(Handle) == FILE_TYPE_CHAR && GetConsoleMode(Handle, &Mode))
    {
        return std::unique_ptr<ConsoleInput>(new ConsoleInput(Handle, Mode));
    }

    return nullptr;
}

ConsoleInput::ConsoleInput(HANDLE Handle, DWORD SavedMode) : m_Handle(Handle), m_SavedMode(SavedMode)
{
    // Save code page
    m_SavedCodePage = GetConsoleCP();

    // Configure for raw input with VT support
    DWORD NewMode = m_SavedMode;
    WI_SetAllFlags(NewMode, ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
    WI_ClearAllFlags(NewMode, ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    TrySetConsoleMode(Handle, NewMode);

    // Set UTF-8 code page
    LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(CP_UTF8));
}

ConsoleInput::~ConsoleInput()
{
    if (m_Handle)
    {
        TrySetConsoleMode(m_Handle, m_SavedMode);
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleCP(m_SavedCodePage));
    }
}

// ConsoleOutput implementation
std::unique_ptr<ConsoleOutput> ConsoleOutput::Create()
{
    wil::unique_hfile ConsoleHandle(
        CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));

    if (!ConsoleHandle)
    {
        return nullptr;
    }

    DWORD Mode;
    if (GetConsoleMode(ConsoleHandle.get(), &Mode))
    {
        return std::unique_ptr<ConsoleOutput>(new ConsoleOutput(std::move(ConsoleHandle), Mode));
    }

    return nullptr;
}

ConsoleOutput::ConsoleOutput(wil::unique_hfile ConsoleHandle, DWORD SavedMode) :
    m_ConsoleHandle(std::move(ConsoleHandle)), m_SavedMode(SavedMode)
{
    // Save code page
    m_SavedCodePage = GetConsoleOutputCP();

    // Configure for VT output with DISABLE_NEWLINE_AUTO_RETURN
    DWORD NewMode = m_SavedMode;
    WI_SetAllFlags(NewMode, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);

    // Try with DISABLE_NEWLINE_AUTO_RETURN first, fall back without it if not supported
    if (!SetConsoleMode(m_ConsoleHandle.get(), NewMode))
    {
        WI_ClearFlag(NewMode, DISABLE_NEWLINE_AUTO_RETURN);
        TrySetConsoleMode(m_ConsoleHandle.get(), NewMode);
    }

    // Set UTF-8 code page
    LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(CP_UTF8));
}

ConsoleOutput::~ConsoleOutput()
{
    if (m_ConsoleHandle)
    {
        TrySetConsoleMode(m_ConsoleHandle.get(), m_SavedMode);
        LOG_IF_WIN32_BOOL_FALSE(SetConsoleOutputCP(m_SavedCodePage));
    }
}

// SvcCommIo implementation
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
    const bool IsConsoleInput = m_ConsoleInput != nullptr;
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
