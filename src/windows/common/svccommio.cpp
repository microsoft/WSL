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
void ChangeConsoleMode(_In_ HANDLE File, _In_ DWORD ConsoleMode)
{
    //
    // Use the invalid parameter error code to detect the v1 console that does
    // not support the provided mode. This can be improved in the future when
    // a more elegant solution exists.
    //
    // N.B. Ignore failures setting the mode if the console has already
    //      disconnected.
    //

    if (!SetConsoleMode(File, ConsoleMode))
    {
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

void ConfigureStdHandles(_Inout_ LXSS_STD_HANDLES_INFO& StdHandlesInfo)
{
    //
    // Check stdin to see if it is a console or another device. If it is
    // a console, configure it to raw processing mode and VT-100 support. If the
    // force console I/O is requested, ignore stdin and get active console input
    // handle instead.
    //

    UINT NewConsoleInputCP = 0;
    DWORD NewConsoleInputMode = 0;
    BOOLEAN IsConsoleInput = StdHandlesInfo.IsConsoleInput;
    BOOLEAN IsConsoleOutput = StdHandlesInfo.IsConsoleOutput;
    BOOLEAN IsConsoleError = StdHandlesInfo.IsConsoleError;
    DWORD SavedInputMode = StdHandlesInfo.SavedInputMode;
    DWORD SavedOutputMode = StdHandlesInfo.SavedOutputMode;
    UINT SavedInputCP = StdHandlesInfo.SavedInputCP;
    UINT SavedOutputCP = StdHandlesInfo.SavedOutputCP;
    CONSOLE_SCREEN_BUFFER_INFO ScreenBufferInfo;
    auto RestoreInputHandle = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        if (NewConsoleInputCP != 0)
        {
            SetConsoleCP(SavedInputCP);
        }

        if (NewConsoleInputMode != 0)
        {
            ChangeConsoleMode(StdHandlesInfo.InputHandle, SavedInputMode);
        }
    });

    IsConsoleInput = FALSE;
    if ((GetFileType(StdHandlesInfo.InputHandle) == FILE_TYPE_CHAR) && (GetConsoleMode(StdHandlesInfo.InputHandle, &SavedInputMode)))
    {
        IsConsoleInput = TRUE;
        NewConsoleInputMode = SavedInputMode;
        WI_SetAllFlags(NewConsoleInputMode, (ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT));
        WI_ClearAllFlags(NewConsoleInputMode, (ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT));
        ChangeConsoleMode(StdHandlesInfo.InputHandle, NewConsoleInputMode);

        //
        // Set the console input to the UTF-8 code page.
        //

        SavedInputCP = GetConsoleCP();
        NewConsoleInputCP = CP_UTF8;
        THROW_LAST_ERROR_IF(!::SetConsoleCP(NewConsoleInputCP));
    }

    bool RestoreMode = false;
    bool RestoreCp = false;
    auto RestoreOutput = wil::scope_exit([&] {
        if (RestoreMode)
        {
            SetConsoleMode(StdHandlesInfo.ConsoleOutputHandle.get(), SavedOutputMode);
        }

        if (RestoreCp)
        {
            SetConsoleOutputCP(SavedOutputCP);
        }
    });

    //
    // If there is a console output handle, save the output mode and codepage so
    // it can be restored.
    //

    if (StdHandlesInfo.ConsoleOutputHandle)
    {
        THROW_LAST_ERROR_IF(!::GetConsoleMode(StdHandlesInfo.ConsoleOutputHandle.get(), &SavedOutputMode));

        //
        // Temporarily try both with and without the custom flag to disable newline
        // auto return.
        //

        DWORD NewConsoleOutputMode = SavedOutputMode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
        if (SetConsoleMode(StdHandlesInfo.ConsoleOutputHandle.get(), NewConsoleOutputMode) == FALSE)
        {
            WI_ClearFlag(NewConsoleOutputMode, DISABLE_NEWLINE_AUTO_RETURN);
            ChangeConsoleMode(StdHandlesInfo.ConsoleOutputHandle.get(), NewConsoleOutputMode);
        }

        RestoreMode = true;

        //
        // Set the console output to the UTF-8 code page.
        //

        SavedOutputCP = GetConsoleOutputCP();
        THROW_LAST_ERROR_IF(!::SetConsoleOutputCP(CP_UTF8));

        RestoreCp = true;
    }

    //
    // If the force console I/O is requested, ignore stdout and treat the
    // console as the output handle.
    //

    IsConsoleOutput = FALSE;
    if ((GetFileType(StdHandlesInfo.OutputHandle) == FILE_TYPE_CHAR) &&
        (GetConsoleScreenBufferInfo(StdHandlesInfo.OutputHandle, &ScreenBufferInfo)))
    {
        IsConsoleOutput = TRUE;
    }

    IsConsoleError = FALSE;
    if ((GetFileType(StdHandlesInfo.ErrorHandle) == FILE_TYPE_CHAR) &&
        (GetConsoleScreenBufferInfo(StdHandlesInfo.ErrorHandle, &ScreenBufferInfo)))
    {
        IsConsoleError = TRUE;
    }

    RestoreInputHandle.release();
    RestoreOutput.release();
    StdHandlesInfo.IsConsoleInput = IsConsoleInput;
    StdHandlesInfo.IsConsoleOutput = IsConsoleOutput;
    StdHandlesInfo.IsConsoleError = IsConsoleError;
    StdHandlesInfo.SavedInputMode = SavedInputMode;
    StdHandlesInfo.SavedOutputMode = SavedOutputMode;
    StdHandlesInfo.SavedInputCP = SavedInputCP;
    StdHandlesInfo.SavedOutputCP = SavedOutputCP;
}
} // namespace

wsl::windows::common::SvcCommIo::SvcCommIo()
{
    _stdHandlesInfo.InputHandle = GetStdHandle(STD_INPUT_HANDLE);
    _stdHandlesInfo.OutputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    _stdHandlesInfo.ErrorHandle = GetStdHandle(STD_ERROR_HANDLE);
    _stdHandlesInfo.ConsoleOutputHandle.reset(
        CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));

    ConfigureStdHandles(_stdHandlesInfo);
    _stdHandles.StdIn.HandleType = LxssHandleInput;
    _stdHandles.StdIn.Handle = HandleToUlong(_stdHandlesInfo.InputHandle);
    _stdHandles.StdOut.HandleType = LxssHandleOutput;
    _stdHandles.StdOut.Handle = HandleToUlong(_stdHandlesInfo.OutputHandle);
    _stdHandles.StdErr.HandleType = LxssHandleOutput;
    _stdHandles.StdErr.Handle = HandleToUlong(_stdHandlesInfo.ErrorHandle);

    //
    // N.B.: The console handle is not supposed to be closed, it is just copied
    //       from PEB.
    //

    if (_stdHandlesInfo.IsConsoleInput)
    {
        _stdHandles.StdIn.Handle = LXSS_HANDLE_USE_CONSOLE;
        _stdHandles.StdIn.HandleType = LxssHandleConsole;
    }

    if (_stdHandlesInfo.IsConsoleOutput)
    {
        _stdHandles.StdOut.Handle = LXSS_HANDLE_USE_CONSOLE;
        _stdHandles.StdOut.HandleType = LxssHandleConsole;
    }

    if (_stdHandlesInfo.IsConsoleError)
    {
        _stdHandles.StdErr.Handle = LXSS_HANDLE_USE_CONSOLE;
        _stdHandles.StdErr.HandleType = LxssHandleConsole;
    }
}

wsl::windows::common::SvcCommIo::~SvcCommIo()
{
    try
    {
        RestoreConsoleMode();
    }
    CATCH_LOG()
}

PLXSS_STD_HANDLES
wsl::windows::common::SvcCommIo::GetStdHandles()
{
    return &_stdHandles;
}

COORD
wsl::windows::common::SvcCommIo::GetWindowSize() const
{
    CONSOLE_SCREEN_BUFFER_INFOEX Info{};
    Info.cbSize = sizeof(Info);
    if (_stdHandlesInfo.IsConsoleOutput)
    {
        THROW_IF_WIN32_BOOL_FALSE(::GetConsoleScreenBufferInfoEx(_stdHandlesInfo.OutputHandle, &Info));
    }
    else if (_stdHandlesInfo.IsConsoleError)
    {
        THROW_IF_WIN32_BOOL_FALSE(::GetConsoleScreenBufferInfoEx(_stdHandlesInfo.ErrorHandle, &Info));
    }

    return {
        static_cast<short>(Info.srWindow.Right - Info.srWindow.Left + 1), static_cast<short>(Info.srWindow.Bottom - Info.srWindow.Top + 1)};
}

void wsl::windows::common::SvcCommIo::RestoreConsoleMode() const

/*++

Routine Description:

    Restores the saved input/output console mode.

Arguments:

    None.

Return Value:

    None.

--*/

{
    //
    // Restore the console input and output modes.
    //

    if (_stdHandlesInfo.ConsoleOutputHandle)
    {
        ChangeConsoleMode(_stdHandlesInfo.ConsoleOutputHandle.get(), _stdHandlesInfo.SavedOutputMode);
        SetConsoleOutputCP(_stdHandlesInfo.SavedOutputCP);
    }

    if (_stdHandlesInfo.IsConsoleInput != FALSE)
    {
        ChangeConsoleMode(_stdHandlesInfo.InputHandle, _stdHandlesInfo.SavedInputMode);
        SetConsoleCP(_stdHandlesInfo.SavedInputCP);
    }
}
