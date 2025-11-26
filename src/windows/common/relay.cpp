/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    relay.cpp

Abstract:

    This file contains function definitions for relay worker thread routines.

--*/

#include "precomp.h"
#include "relay.hpp"
#pragma hdrstop

using wsl::windows::common::relay::EventHandle;
using wsl::windows::common::relay::IOHandleStatus;
using wsl::windows::common::relay::MultiHandleWait;
using wsl::windows::common::relay::OverlappedIOHandle;
using wsl::windows::common::relay::ReadHandle;
using wsl::windows::common::relay::ScopedMultiRelay;
using wsl::windows::common::relay::ScopedRelay;
using wsl::windows::common::relay::WriteHandle;

namespace {

LARGE_INTEGER InitializeFileOffset(HANDLE File)
{
    LARGE_INTEGER Offset{};
    if (GetFileType(File) == FILE_TYPE_DISK)
    {
        LOG_IF_WIN32_BOOL_FALSE(SetFilePointerEx(File, {}, &Offset, FILE_CURRENT));
    }

    return Offset;
}

} // namespace

std::thread wsl::windows::common::relay::CreateThread(_In_ HANDLE InputHandle, _In_ HANDLE OutputHandle, _In_opt_ HANDLE ExitHandle, _In_ size_t BufferSize)
{
    return std::thread([InputHandle, OutputHandle, ExitHandle, BufferSize]() {
        try
        {
            wsl::windows::common::wslutil::SetThreadDescription(L"IO Relay");
            InterruptableRelay(InputHandle, OutputHandle, ExitHandle, BufferSize);
        }
        CATCH_LOG()
    });
}

std::thread wsl::windows::common::relay::CreateThread(
    _In_ wil::unique_handle&& InputHandle, _In_ HANDLE OutputHandle, _In_opt_ HANDLE ExitHandle, _In_ size_t BufferSize)
{
    return std::thread([InputHandle = std::move(InputHandle), OutputHandle, ExitHandle, BufferSize]() {
        try
        {
            wsl::windows::common::wslutil::SetThreadDescription(L"IO Relay");
            InterruptableRelay(InputHandle.get(), OutputHandle, ExitHandle, BufferSize);
        }
        CATCH_LOG()
    });
}

std::thread wsl::windows::common::relay::CreateThread(
    _In_ HANDLE InputHandle, _In_ wil::unique_handle&& OutputHandle, _In_opt_ HANDLE ExitHandle, _In_ size_t BufferSize)
{
    return std::thread([InputHandle, OutputHandle = std::move(OutputHandle), ExitHandle, BufferSize]() {
        try
        {
            wsl::windows::common::wslutil::SetThreadDescription(L"IO Relay");
            InterruptableRelay(InputHandle, OutputHandle.get(), ExitHandle, BufferSize);
        }
        CATCH_LOG()
    });
}

std::thread wsl::windows::common::relay::CreateThread(
    _In_ wil::unique_handle&& InputHandle, _In_ wil::unique_handle&& OutputHandle, _In_opt_ HANDLE ExitHandle, _In_ size_t BufferSize)
{
    return std::thread([InputHandle = std::move(InputHandle), OutputHandle = std::move(OutputHandle), ExitHandle, BufferSize]() {
        try
        {
            wsl::windows::common::wslutil::SetThreadDescription(L"IO Relay");
            InterruptableRelay(InputHandle.get(), OutputHandle.get(), ExitHandle, BufferSize);
        }
        CATCH_LOG()
    });
}

DWORD
wsl::windows::common::relay::InterruptableRead(
    _In_ HANDLE InputHandle, _In_ gsl::span<gsl::byte> Buffer, _In_ const std::vector<HANDLE>& ExitHandles, _In_opt_ LPOVERLAPPED Overlapped)
{
    // Initialize an overlapped structure if one was not provided by the caller.
    OVERLAPPED overlapped = {};
    wil::unique_event overlappedEvent = {};
    if (!ARGUMENT_PRESENT(Overlapped))
    {
        overlappedEvent.create(wil::EventOptions::ManualReset);
        overlapped.hEvent = overlappedEvent.get();
        Overlapped = &overlapped;
    }

    DWORD bytesRead = 0;
    if (!ReadFile(InputHandle, Buffer.data(), gsl::narrow_cast<DWORD>(Buffer.size()), &bytesRead, Overlapped))
    {
        auto lastError = GetLastError();
        if ((lastError == ERROR_HANDLE_EOF) || (lastError == ERROR_BROKEN_PIPE))
        {
            return 0;
        }

        THROW_LAST_ERROR_IF_MSG(lastError != ERROR_IO_PENDING, "Handle: 0x%p", (void*)InputHandle);

        auto cancelRead = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            CancelIoEx(InputHandle, Overlapped);
            GetOverlappedResult(InputHandle, Overlapped, &bytesRead, TRUE);
        });

        // Wait for the read to complete, or the client to exit.
        if (!InterruptableWait(Overlapped->hEvent, ExitHandles))
        {
            return 0;
        }

        if (!GetOverlappedResult(InputHandle, Overlapped, &bytesRead, FALSE))
        {
            lastError = GetLastError();
            if ((lastError == ERROR_HANDLE_EOF) || (lastError == ERROR_BROKEN_PIPE))
            {
                return 0;
            }

            THROW_LAST_ERROR();
        }

        cancelRead.release();
    }

    return bytesRead;
}

void wsl::windows::common::relay::InterruptableRelay(_In_ HANDLE InputHandle, _In_opt_ HANDLE OutputHandle, _In_opt_ HANDLE ExitHandle, _In_ size_t BufferSize)
{
    // If the handle file is seekable, make sure to respect the offset.
    // This is useful in cases when WSL is invoked on an existing file, like: wsl.exe echo foo >> file
    // See: https://github.com/microsoft/WSL/issues/11799

    LARGE_INTEGER writeOffset = InitializeFileOffset(OutputHandle);
    LARGE_INTEGER readOffset = InitializeFileOffset(InputHandle);

    std::vector<gsl::byte> buffer(BufferSize);
    const auto readSpan = gsl::make_span(buffer);

    std::vector<HANDLE> exitHandles;
    if (ExitHandle)
    {
        exitHandles.push_back(ExitHandle);
    }

    OVERLAPPED overlapped = {0};
    const wil::unique_event overlappedEvent(wil::EventOptions::ManualReset);
    overlapped.hEvent = overlappedEvent.get();
    for (;;)
    {
        overlapped.Offset = readOffset.LowPart;
        overlapped.OffsetHigh = readOffset.HighPart;
        const auto bytesRead = InterruptableRead(InputHandle, readSpan, exitHandles, &overlapped);
        if (bytesRead == 0)
        {
            break;
        }

        readOffset.QuadPart += bytesRead;

        if (OutputHandle)
        {
            overlapped.Offset = writeOffset.LowPart;
            overlapped.OffsetHigh = writeOffset.HighPart;
            auto writeSpan = readSpan.first(bytesRead);
            const auto bytesWritten = InterruptableWrite(OutputHandle, writeSpan, exitHandles, &overlapped);
            if (bytesWritten == 0)
            {
                break;
            }

            WI_ASSERT(bytesWritten == bytesRead);
        }

        writeOffset.QuadPart += bytesRead;
    }
}

bool wsl::windows::common::relay::InterruptableWait(_In_ HANDLE WaitObject, _In_ const std::vector<HANDLE>& ExitHandles)
{
    // Wait for the object to become signaled or one of the exit handles to be signaled.
    std::vector<HANDLE> waitObjects{WaitObject};
    for (const auto& exitHandle : ExitHandles)
    {
        waitObjects.push_back(exitHandle);
    }

    const DWORD waitResult = WaitForMultipleObjects(gsl::narrow_cast<DWORD>(waitObjects.size()), waitObjects.data(), FALSE, INFINITE);
    if (waitResult != WAIT_OBJECT_0)
    {
        if (waitResult > WAIT_OBJECT_0 && waitResult <= WAIT_OBJECT_0 + waitObjects.size())
        {
            return false;
        }

        THROW_HR_MSG(E_FAIL, "WaitForMultipleObjects %d", waitResult);
    }

    return true;
}

DWORD
wsl::windows::common::relay::InterruptableWrite(
    _In_ HANDLE OutputHandle, _In_ gsl::span<const gsl::byte> Buffer, _In_ const std::vector<HANDLE>& ExitHandles, _In_ LPOVERLAPPED Overlapped)
{
    const DWORD bytesToWrite = gsl::narrow_cast<DWORD>(Buffer.size());
    DWORD bytesWritten = 0;
    BOOL success = WriteFile(OutputHandle, Buffer.data(), bytesToWrite, &bytesWritten, Overlapped);
    if (!success)
    {
        const auto lastError = GetLastError();
        if (lastError == ERROR_NO_DATA)
        {
            return 0;
        }

        THROW_LAST_ERROR_IF(lastError != ERROR_IO_PENDING);

        auto cancelWrite = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            CancelIoEx(OutputHandle, Overlapped);
            GetOverlappedResult(OutputHandle, Overlapped, &bytesWritten, TRUE);
        });

        if (InterruptableWait(Overlapped->hEvent, ExitHandles))
        {
            success = GetOverlappedResult(OutputHandle, Overlapped, &bytesWritten, FALSE);
            if (success)
            {
                cancelWrite.release();
            }
        }
    }

    WI_ASSERT(!success || (bytesWritten == bytesToWrite));

    return bytesWritten;
}

void wsl::windows::common::relay::BidirectionalRelay(_In_ HANDLE LeftHandle, _In_ HANDLE RightHandle, _In_ size_t BufferSize, _In_ RelayFlags Flags)
{
    std::vector<gsl::byte> leftBuffer(BufferSize);
    const auto leftReadSpan = gsl::make_span(leftBuffer);
    OVERLAPPED leftOverlapped = {0};
    const wil::unique_event leftOverlappedEvent(wil::EventOptions::None);
    leftOverlapped.hEvent = leftOverlappedEvent.get();
    LARGE_INTEGER leftOffset{};

    std::vector<gsl::byte> rightBuffer(BufferSize);
    const auto rightReadSpan = gsl::make_span(rightBuffer);
    OVERLAPPED rightOverlapped = {0};
    const wil::unique_event rightOverlappedEvent(wil::EventOptions::None);
    rightOverlapped.hEvent = rightOverlappedEvent.get();
    LARGE_INTEGER rightOffset{};

    bool leftReadPending = false;
    bool rightReadPending = false;
    auto cancelReads = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        DWORD bytes;
        if (leftReadPending)
        {
            CancelIoEx(LeftHandle, &leftOverlapped);
            GetOverlappedResult(LeftHandle, &leftOverlapped, &bytes, TRUE);
        }

        if (rightReadPending)
        {
            CancelIoEx(RightHandle, &rightOverlapped);
            GetOverlappedResult(RightHandle, &rightOverlapped, &bytes, TRUE);
        }
    });

    DWORD bytesWritten;
    const HANDLE waitObjects[] = {leftOverlapped.hEvent, rightOverlapped.hEvent};
    for (;;)
    {
        if ((LeftHandle == nullptr) || (RightHandle == nullptr))
        {
            break;
        }

        DWORD leftBytesRead = 0;
        if (!leftReadPending && LeftHandle)
        {
            if (!ReadFile(LeftHandle, leftReadSpan.data(), gsl::narrow_cast<DWORD>(leftReadSpan.size()), &leftBytesRead, &leftOverlapped))
            {
                THROW_LAST_ERROR_IF(GetLastError() != ERROR_IO_PENDING);
            }

            leftReadPending = true;
        }

        DWORD rightBytesRead = 0;
        if (!rightReadPending && RightHandle)
        {
            if (!ReadFile(RightHandle, rightReadSpan.data(), gsl::narrow_cast<DWORD>(rightReadSpan.size()), &rightBytesRead, &rightOverlapped))
            {
                THROW_LAST_ERROR_IF(GetLastError() != ERROR_IO_PENDING);
            }

            rightReadPending = true;
        }

        const DWORD waitResult = WaitForMultipleObjects(RTL_NUMBER_OF(waitObjects), waitObjects, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0)
        {
            LOG_LAST_ERROR_IF_MSG(
                !GetOverlappedResult(LeftHandle, &leftOverlapped, &leftBytesRead, FALSE), "WSAGetLastError %d", WSAGetLastError());

            leftReadPending = false;
            if (leftBytesRead == 0)
            {
                LeftHandle = nullptr;
                if (WI_IsFlagSet(Flags, RelayFlags::RightIsSocket))
                {
                    LOG_LAST_ERROR_IF(shutdown(reinterpret_cast<SOCKET>(RightHandle), SD_SEND) == SOCKET_ERROR);
                }
            }
            else if (RightHandle != nullptr)
            {
                auto writeSpan = leftReadSpan.first(leftBytesRead);
                bytesWritten = InterruptableWrite(RightHandle, writeSpan, {}, &leftOverlapped);
                if (bytesWritten == 0)
                {
                    break;
                }

                leftOffset.QuadPart += leftBytesRead;
                leftOverlapped.Offset = leftOffset.LowPart;
                leftOverlapped.OffsetHigh = leftOffset.HighPart;
            }
        }
        else if (waitResult == (WAIT_OBJECT_0 + 1))
        {
            LOG_LAST_ERROR_IF_MSG(
                !GetOverlappedResult(RightHandle, &rightOverlapped, &rightBytesRead, FALSE), "WSAGetLastError %d", WSAGetLastError());

            rightReadPending = false;
            if (rightBytesRead == 0)
            {
                RightHandle = nullptr;
                if (WI_IsFlagSet(Flags, RelayFlags::LeftIsSocket))
                {
                    LOG_LAST_ERROR_IF(shutdown(reinterpret_cast<SOCKET>(LeftHandle), SD_SEND) == SOCKET_ERROR);
                }
            }
            else if (LeftHandle != nullptr)
            {
                auto writeSpan = rightReadSpan.first(rightBytesRead);
                bytesWritten = InterruptableWrite(LeftHandle, writeSpan, {}, &rightOverlapped);
                if (bytesWritten == 0)
                {
                    break;
                }

                rightOffset.QuadPart += rightBytesRead;
                rightOverlapped.Offset = rightOffset.LowPart;
                rightOverlapped.OffsetHigh = rightOffset.HighPart;
            }
        }
        else
        {
            THROW_HR_MSG(E_FAIL, "WaitForMultipleObjects %d", waitResult);
        }
    }
}

#define TTY_ALT_NUMPAD_VK_MENU (0x12)
#define TTY_ESCAPE_CHARACTER (L'\x1b')
#define TTY_INPUT_EVENT_BUFFER_SIZE (16)
#define TTY_UTF8_TRANSLATION_BUFFER_SIZE (4 * TTY_INPUT_EVENT_BUFFER_SIZE)

BOOL IsActionableKey(_In_ PKEY_EVENT_RECORD KeyEvent)
{
    //
    // This is a bit complicated to discern.
    //
    // 1. Our first check is that we only want structures that
    //    represent at least one key press. If we have 0, then we don't
    //    need to bother. If we have >1, we'll send the key through
    //    that many times into the pipe.
    // 2. Our second check is where it gets confusing.
    //    a. Characters that are non-null get an automatic pass. Copy
    //       them through to the pipe.
    //    b. Null characters need further scrutiny. We generally do not
    //       pass nulls through EXCEPT if they're sourced from the
    //       virtual terminal engine (or another application living
    //       above our layer). If they're sourced by a non-keyboard
    //       source, they'll have no scan code (since they didn't come
    //       from a keyboard). But that rule has an exception too:
    //       "Enhanced keys" from above the standard range of scan
    //       codes will return 0 also with a special flag set that says
    //       they're an enhanced key. That means the desired behavior
    //       is:
    //           Scan Code = 0, ENHANCED_KEY = 0
    //               -> This came from the VT engine or another app
    //                  above our layer.
    //           Scan Code = 0, ENHANCED_KEY = 1
    //               -> This came from the keyboard, but is a special
    //                  key like 'Volume Up' that wasn't generally a
    //                  part of historic (pre-1990s) keyboards.
    //           Scan Code = <anything else>
    //               -> This came from a keyboard directly.
    //

    if ((KeyEvent->wRepeatCount == 0) || ((KeyEvent->uChar.UnicodeChar == UNICODE_NULL) &&
                                          ((KeyEvent->wVirtualScanCode != 0) || (WI_IsFlagSet(KeyEvent->dwControlKeyState, ENHANCED_KEY)))))
    {
        return FALSE;
    }

    return TRUE;
}

BOOL GetNextCharacter(_In_ INPUT_RECORD* InputRecord, _Out_ PWCHAR NextCharacter)
{
    BOOL IsNextCharacterValid = FALSE;
    if (InputRecord->EventType == KEY_EVENT)
    {
        const auto KeyEvent = &InputRecord->Event.KeyEvent;
        if ((IsActionableKey(KeyEvent) != FALSE) && ((KeyEvent->bKeyDown != FALSE) || (KeyEvent->wVirtualKeyCode == TTY_ALT_NUMPAD_VK_MENU)))
        {
            *NextCharacter = KeyEvent->uChar.UnicodeChar;
            IsNextCharacterValid = TRUE;
        }
    }

    return IsNextCharacterValid;
}

void wsl::windows::common::relay::StandardInputRelay(HANDLE ConsoleHandle, HANDLE OutputHandle, const std::function<void()>& UpdateTerminalSize, HANDLE ExitEvent)
try
{
    if (GetFileType(ConsoleHandle) != FILE_TYPE_CHAR)
    {
        wsl::windows::common::relay::InterruptableRelay(ConsoleHandle, OutputHandle, ExitEvent);
        return;
    }

    //
    // N.B. ReadConsoleInputEx has no associated import library.
    //

    static LxssDynamicFunction<decltype(ReadConsoleInputExW)> readConsoleInput(L"Kernel32.dll", "ReadConsoleInputExW");

    INPUT_RECORD InputRecordBuffer[TTY_INPUT_EVENT_BUFFER_SIZE];
    INPUT_RECORD* InputRecordPeek = &(InputRecordBuffer[1]);
    KEY_EVENT_RECORD* KeyEvent;
    DWORD RecordsRead;
    OVERLAPPED Overlapped = {0};
    const wil::unique_event OverlappedEvent(wil::EventOptions::ManualReset);
    Overlapped.hEvent = OverlappedEvent.get();
    const HANDLE WaitHandles[] = {ExitEvent, ConsoleHandle};
    const std::vector<HANDLE> ExitHandles = {ExitEvent};
    for (;;)
    {
        //
        // Because some input events generated by the console are encoded with
        // more than one input event, we have to be smart about reading the
        // events.
        //
        // First, we peek at the next input event.
        // If it's an escape (wch == L'\x1b') event, then the characters that
        //      follow are part of an input sequence. We can't know for sure
        //      how long that sequence is, but we can assume it's all sent to
        //      the input queue at once, and it's less that 16 events.
        //      Furthermore, we can assume that if there's an Escape in those
        //      16 events, that the escape marks the start of a new sequence.
        //      So, we'll peek at another 15 events looking for escapes.
        //      If we see an escape, then we'll read one less than that,
        //      such that the escape remains the next event in the input.
        //      From those read events, we'll aggregate chars into a single
        //      string to send to the subsystem.
        // If it's not an escape, send the event through one at a time.
        //

        //
        // Read one input event.
        //

        DWORD WaitStatus = (WAIT_OBJECT_0 + 1);
        do
        {
            THROW_IF_WIN32_BOOL_FALSE(readConsoleInput(ConsoleHandle, InputRecordBuffer, 1, &RecordsRead, CONSOLE_READ_NOWAIT));

            if (RecordsRead == 0)
            {
                WaitStatus = WaitForMultipleObjects(RTL_NUMBER_OF(WaitHandles), WaitHandles, false, INFINITE);
            }
        } while ((WaitStatus == (WAIT_OBJECT_0 + 1)) && (RecordsRead == 0));

        //
        // Stop processing if the exit event has been signaled.
        //

        if (WaitStatus != (WAIT_OBJECT_0 + 1))
        {
            WI_ASSERT(WaitStatus == WAIT_OBJECT_0);

            break;
        }

        WI_ASSERT(RecordsRead == 1);

        //
        // Don't read additional records if the first entry is a window size
        // event, or a repeated character. Handle those events on their own.
        //

        DWORD RecordsPeeked = 0;
        if ((InputRecordBuffer[0].EventType != WINDOW_BUFFER_SIZE_EVENT) &&
            ((InputRecordBuffer[0].EventType != KEY_EVENT) || (InputRecordBuffer[0].Event.KeyEvent.wRepeatCount < 2)))
        {
            //
            // Read additional input records into the buffer if available.
            //

            THROW_IF_WIN32_BOOL_FALSE(PeekConsoleInputW(ConsoleHandle, InputRecordPeek, (RTL_NUMBER_OF(InputRecordBuffer) - 1), &RecordsPeeked));
        }

        //
        // Iterate over peeked records [1, RecordsPeeked].
        //

        DWORD AdditionalRecordsToRead = 0;
        WCHAR NextCharacter;
        for (DWORD RecordIndex = 1; RecordIndex <= RecordsPeeked; RecordIndex++)
        {
            if (GetNextCharacter(&InputRecordBuffer[RecordIndex], &NextCharacter) != FALSE)
            {
                KeyEvent = &InputRecordBuffer[RecordIndex].Event.KeyEvent;
                if (NextCharacter == TTY_ESCAPE_CHARACTER)
                {
                    //
                    // CurrentRecord is an escape event. We will start here
                    // on the next input loop.
                    //

                    break;
                }
                else if (KeyEvent->wRepeatCount > 1)
                {
                    //
                    // Repeated keys are handled on their own. Start with this
                    // key on the next input loop.
                    //

                    break;
                }
                else if (IS_HIGH_SURROGATE(NextCharacter) && (RecordIndex >= (RecordsPeeked - 1)))
                {
                    //
                    // If there is not enough room for the second character of
                    // a surrogate pair, start with this character on the next
                    // input loop.
                    //
                    // N.B. The test is for at least two remaining records
                    //      because typically a surrogate pair will be entered
                    //      via copy/paste, which will appear as an input
                    //      record with alt-down, alt-up and character. So to
                    //      include the next character of the surrogate pair it
                    //      is likely that the alt-up record will need to be
                    //      read first.
                    //

                    break;
                }
            }
            else if (InputRecordBuffer[RecordIndex].EventType == WINDOW_BUFFER_SIZE_EVENT)
            {
                //
                // A window size event is handled on its own.
                //

                break;
            }

            //
            // Process the additional input record.
            //

            AdditionalRecordsToRead += 1;
        }

        if (AdditionalRecordsToRead > 0)
        {
            THROW_IF_WIN32_BOOL_FALSE(readConsoleInput(ConsoleHandle, InputRecordPeek, AdditionalRecordsToRead, &RecordsRead, CONSOLE_READ_NOWAIT));

            if (RecordsRead == 0)
            {
                //
                // This would be an unexpected case. We've already peeked to see
                // that there are AdditionalRecordsToRead # of records in the
                // input that need reading, yet we didn't get them when we read.
                // In this case, move along and finish this input event.
                //

                break;
            }

            //
            // We already had one input record in the buffer before reading
            // additional, So account for that one too
            //

            RecordsRead += 1;
        }

        //
        // Process each input event. Keydowns will get aggregated into
        // Utf8String before getting injected into the subsystem.
        //

        WCHAR Utf16String[TTY_INPUT_EVENT_BUFFER_SIZE];
        ULONG Utf16StringSize = 0;
        COORD WindowSize{};
        for (DWORD RecordIndex = 0; RecordIndex < RecordsRead; RecordIndex++)
        {
            INPUT_RECORD* CurrentInputRecord = &(InputRecordBuffer[RecordIndex]);
            switch (CurrentInputRecord->EventType)
            {
            case KEY_EVENT:

                //
                // Filter out key up events unless they are from an <Alt> key.
                // Key up with an <Alt> key could contain a Unicode character
                // pasted from the clipboard and converted to an <Alt>+<Numpad> sequence.
                //

                KeyEvent = &CurrentInputRecord->Event.KeyEvent;
                if ((KeyEvent->bKeyDown == FALSE) && (KeyEvent->wVirtualKeyCode != TTY_ALT_NUMPAD_VK_MENU))
                {
                    break;
                }

                //
                // Filter out key presses that are not actionable, such as just
                // pressing <Ctrl>, <Alt>, <Shift> etc. These key presses return
                // the character of null but will have a valid scan code off the
                // keyboard. Certain other key sequences such as Ctrl+A,
                // Ctrl+<space>, and Ctrl+@ will also return the character null
                // but have no scan code.
                // <Alt> + <NumPad> sequences will show an <Alt> but will have
                // a scancode and character specified, so they should be actionable.
                //

                if (IsActionableKey(KeyEvent) == FALSE)
                {
                    break;
                }

                Utf16String[Utf16StringSize] = KeyEvent->uChar.UnicodeChar;
                Utf16StringSize += 1;
                break;

            case WINDOW_BUFFER_SIZE_EVENT:

                //
                // Query the window size and send an update message via the
                // control channel.
                //

                UpdateTerminalSize();
                break;
            }
        }

        CHAR Utf8String[TTY_UTF8_TRANSLATION_BUFFER_SIZE];
        DWORD Utf8StringSize = 0;
        if (Utf16StringSize > 0)
        {
            //
            // Windows uses UTF-16LE encoding, Linux uses UTF-8 by default.
            // Convert each UTF-16LE character into the proper UTF-8 byte
            // sequence equivalent.
            //

            THROW_LAST_ERROR_IF(
                (Utf8StringSize = WideCharToMultiByte(
                     CP_UTF8, 0, Utf16String, Utf16StringSize, Utf8String, sizeof(Utf8String), nullptr, nullptr)) == 0);
        }

        //
        // Send the input bytes to the terminal.
        //

        DWORD BytesWritten = 0;
        const auto Utf8Span = gslhelpers::struct_as_bytes(Utf8String).first(Utf8StringSize);
        if ((RecordsRead == 1) && (InputRecordBuffer[0].EventType == KEY_EVENT) && (InputRecordBuffer[0].Event.KeyEvent.wRepeatCount > 1))
        {
            WI_ASSERT(Utf16StringSize == 1);

            //
            // Handle repeated characters. They aren't part of an input
            // sequence, so there's only one event that's generating characters.
            //

            WORD RepeatIndex;
            for (RepeatIndex = 0; RepeatIndex < InputRecordBuffer[0].Event.KeyEvent.wRepeatCount; RepeatIndex += 1)
            {
                BytesWritten = wsl::windows::common::relay::InterruptableWrite(OutputHandle, Utf8Span, ExitHandles, &Overlapped);
                if (BytesWritten == 0)
                {
                    break;
                }
            }
        }
        else if (Utf8StringSize > 0)
        {
            BytesWritten = wsl::windows::common::relay::InterruptableWrite(OutputHandle, Utf8Span, ExitHandles, &Overlapped);
            if (BytesWritten == 0)
            {
                break;
            }
        }
    }

    return;
}
CATCH_LOG()

void wsl::windows::common::relay::SocketRelay(_In_ SOCKET LeftSocket, _In_ SOCKET RightSocket, _In_ size_t BufferSize)
{
    constexpr RelayFlags flags = RelayFlags::LeftIsSocket | RelayFlags::RightIsSocket;
    BidirectionalRelay(reinterpret_cast<HANDLE>(LeftSocket), reinterpret_cast<HANDLE>(RightSocket), BufferSize, flags);
}

void ScopedRelay::Sync()
{
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

ScopedRelay::~ScopedRelay()
{
    try
    {
        m_onDestroy();
    }
    CATCH_LOG();

    m_exitEvent.SetEvent();
    Sync();
}

void ScopedRelay::Run(_In_ HANDLE Input, _In_ HANDLE Output, size_t BufferSize) const
{
    wsl::windows::common::wslutil::SetThreadDescription(L"ScopedRelay");

    try
    {
        InterruptableRelay(Input, Output, m_exitEvent.get(), BufferSize);
    }
    CATCH_LOG();
}

ScopedMultiRelay::ScopedMultiRelay(const std::vector<HANDLE>& Inputs, const TWriteMethod& Write, size_t BufferSize)
{
    m_thread = std::thread{[this, BufferSize = BufferSize, Inputs = std::move(Inputs), Write = std::move(Write)]() {
        Run(Inputs, Write, BufferSize);
    }};
}

void ScopedMultiRelay::Sync()
{
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

ScopedMultiRelay::~ScopedMultiRelay()
{
    m_exitEvent.SetEvent();
    Sync();
}

void ScopedMultiRelay::Run(const std::vector<HANDLE>& Handles, const TWriteMethod& Write, size_t BufferSize) const
try
{
    enum State
    {
        Standby,
        Pending,
        Eof
    };

    struct Input
    {
        HANDLE Handle;
        LARGE_INTEGER Offset;
        std::vector<std::byte> Buffer;
        wil::unique_event Event{wil::EventOptions::ManualReset};
        OVERLAPPED Overlapped;
        State State = Standby;

        Input(Input&&) = default;
        Input& operator=(Input&&) = default;

        Input(HANDLE Handle, LARGE_INTEGER Offset, size_t BufferSize) : Handle(Handle), Offset(Offset), Buffer(BufferSize)
        {
            Overlapped.hEvent = Event.get();
        }

        ~Input()
        {
            // Cancel outstanding IO, if any.
            if (State == Pending)
            {
                CancelIoEx(Handle, &Overlapped);
                DWORD bytesRead{};
                GetOverlappedResult(Handle, &Overlapped, &bytesRead, TRUE);
            }
        }
    };

    std::vector<Input> Inputs;
    for (const auto& e : Handles)
    {
        Inputs.emplace_back(e, InitializeFileOffset(e), BufferSize);
    }

    while (true)
    {
        // Exit if all inputs are completed, or if the exit event is set.
        if (m_exitEvent.is_signaled() || std::all_of(Inputs.begin(), Inputs.end(), [](const auto& e) { return e.State == Eof; }))
        {
            return;
        }

        for (size_t i = 0; i < Inputs.size(); i++)
        {
            auto& e = Inputs[i];

            // If a read has been scheduled, check if IO is available.
            if (e.State == Pending)
            {
                if (e.Event.is_signaled())
                {
                    DWORD Transferred{};
                    if (!GetOverlappedResult(e.Handle, &e.Overlapped, &Transferred, TRUE))
                    {
                        auto lastError = GetLastError();
                        if ((lastError == ERROR_HANDLE_EOF) || (lastError == ERROR_BROKEN_PIPE))
                        {
                            e.State = Eof;
                            continue;
                        }

                        THROW_LAST_ERROR_IF(lastError != ERROR_IO_PENDING);
                    }

                    // IO is available.
                    Write(i, gsl::make_span(e.Buffer.data(), Transferred));

                    // Update input state.
                    e.Offset.QuadPart += Transferred;
                    e.State = Standby;
                }
            }

            // If no read is pending, start one.
            if (e.State == Standby)
            {
                e.Event.ResetEvent();

                e.Overlapped.Offset = e.Offset.LowPart;
                e.Overlapped.OffsetHigh = e.Offset.HighPart;

                DWORD BytesRead{};
                if (ReadFile(e.Handle, e.Buffer.data(), static_cast<DWORD>(e.Buffer.size()), &BytesRead, &e.Overlapped))
                {
                    // IO is available.
                    Write(i, gsl::make_span(e.Buffer.data(), BytesRead));

                    // Update input state.
                    e.Offset.QuadPart += BytesRead;
                    e.State = Standby;
                }
                else
                {
                    auto lastError = GetLastError();
                    if ((lastError == ERROR_HANDLE_EOF) || (lastError == ERROR_BROKEN_PIPE))
                    {
                        e.State = Eof;
                        continue;
                    }

                    THROW_LAST_ERROR_IF(lastError != ERROR_IO_PENDING);
                    e.State = Pending;
                }
            }
        }

        // Only wait if all non-completed inputs have a scheduled ReadFile to avoid a pipe hang.
        if (std::all_of(Inputs.begin(), Inputs.end(), [](const auto& e) { return e.State == Eof || e.State == Pending; }))
        {
            // Wait until a handle is signaled.
            std::vector<HANDLE> waits{m_exitEvent.get()};
            for (const auto& e : Inputs)
            {
                if (e.State == Pending)
                {
                    waits.emplace_back(e.Event.get());
                }
            }

            THROW_LAST_ERROR_IF(WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), false, INFINITE) == WAIT_FAILED);
        }
    }
}
CATCH_LOG()

void MultiHandleWait::AddHandle(std::unique_ptr<OverlappedIOHandle>&& handle)
{
    m_handles.emplace_back(std::move(handle));
}

void MultiHandleWait::Run(std::optional<std::chrono::milliseconds> Timeout)
{
    std::optional<std::chrono::steady_clock::time_point> deadline;

    if (Timeout.has_value())
    {
        deadline = std::chrono::steady_clock::now() + Timeout.value();
    }

    // Run until all handles are completed.

    while (!m_handles.empty())
    {
        // Schedule IO on each handle until all are either pending, or completed.
        for (auto i = 0; i < m_handles.size(); i++)
        {
            while (m_handles[i]->GetState() == IOHandleStatus::Standby)
            {
                m_handles[i]->Schedule();
            }
        }

        // Remove completed handles from m_handles.
        std::erase_if(m_handles, [&](const auto& e) { return e->GetState() == IOHandleStatus::Completed; });

        if (m_handles.empty())
        {
            break;
        }

        // Wait for the next operation to complete.
        std::vector<HANDLE> waitHandles;
        for (const auto& e : m_handles)
        {
            waitHandles.emplace_back(e->GetHandle());
        }

        DWORD waitTimeout = INFINITE;
        if (deadline.has_value())
        {
            auto miliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline.value() - std::chrono::steady_clock::now()).count();

            waitTimeout = static_cast<DWORD>(std::max(0LL, miliseconds));
        }

        auto result = WaitForMultipleObjects(static_cast<DWORD>(waitHandles.size()), waitHandles.data(), false, waitTimeout);
        if (result == WAIT_TIMEOUT)
        {
            THROW_WIN32(ERROR_TIMEOUT);
        }
        else if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + m_handles.size())
        {
            auto index = result - WAIT_OBJECT_0;
            m_handles[index]->Collect();
        }
        else
        {
            THROW_LAST_ERROR_MSG("Timeout: %lu, Count: %llu", waitTimeout, waitHandles.size());
        }
    }
}

IOHandleStatus OverlappedIOHandle::GetState() const
{
    return State;
}

EventHandle::EventHandle(wil::unique_event&& Handle, std::function<void()>&& OnSignalled) :
    Handle(std::move(Handle)), OnSignalled(std::move(OnSignalled))
{
}

void EventHandle::Schedule()
{
    State = IOHandleStatus::Pending;
}

void EventHandle::Collect()
{
    State = IOHandleStatus::Completed;
    OnSignalled();
}

HANDLE EventHandle::GetHandle() const
{
    return Handle.get();
}

ReadHandle::ReadHandle(wil::unique_handle&& MovedHandle, std::function<void(const gsl::span<char>& Buffer)>&& OnRead) :
    Handle(std::move(MovedHandle)), OnRead(OnRead)
{
    Overlapped.hEvent = Event.get();
}

ReadHandle::~ReadHandle()
{
    if (State == IOHandleStatus::Pending)
    {
        DWORD bytesRead{};
        LOG_IF_WIN32_BOOL_FALSE(CancelIoEx(Handle.get(), &Overlapped));
        LOG_IF_WIN32_BOOL_FALSE(GetOverlappedResult(Handle.get(), &Overlapped, &bytesRead, true));
    }
}

void ReadHandle::Schedule()
{
    WI_ASSERT(State == IOHandleStatus::Standby);

    Event.ResetEvent();

    // Schedule the read.
    DWORD bytesRead{};
    if (ReadFile(Handle.get(), Buffer.data(), static_cast<DWORD>(Buffer.size()), &bytesRead, &Overlapped))
    {
        // Signal the read.
        OnRead(gsl::make_span<char>(Buffer.data(), static_cast<size_t>(bytesRead)));

        // ReadFile completed immediately, process the result right away.
        if (bytesRead == 0)
        {
            State = IOHandleStatus::Completed;
            return; // Handle is completely read, don't try again.
        }

        // Read was done synchronously, remain in 'standby' state.
    }
    else
    {
        auto error = GetLastError();
        if (error == ERROR_HANDLE_EOF || error == ERROR_BROKEN_PIPE)
        {
            State = IOHandleStatus::Completed;
            return;
        }

        THROW_LAST_ERROR_IF_MSG(error != ERROR_IO_PENDING, "Handle: 0x%p", (void*)Handle.get());

        // The read is pending, update to 'Pending'
        State = IOHandleStatus::Pending;
    }
}

void ReadHandle::Collect()
{
    WI_ASSERT(State == IOHandleStatus::Pending);

    // Transition back to standby
    State = IOHandleStatus::Standby;

    // Complete the read.
    DWORD bytesRead{};
    if (!GetOverlappedResult(Handle.get(), &Overlapped, &bytesRead, false))
    {
        auto error = GetLastError();
        THROW_WIN32_IF(error, error != ERROR_HANDLE_EOF && error != ERROR_BROKEN_PIPE);

        // We received ERROR_HANDLE_EOF or ERROR_BROKEN_PIPE. Validate that this was indeed a zero byte read.
        WI_ASSERT(bytesRead == 0);
    }

    // Signal the read.
    OnRead(gsl::make_span<char>(Buffer.data(), static_cast<size_t>(bytesRead)));

    // Transition to Complete if this was a zero byte read.
    if (bytesRead == 0)
    {
        State = IOHandleStatus::Completed;
    }
}

HANDLE ReadHandle::GetHandle() const
{
    return Event.get();
}

WriteHandle::WriteHandle(wil::unique_handle&& MovedHandle, const std::vector<char>& Buffer) :
    Handle(std::move(MovedHandle)), Buffer(Buffer)
{
    Overlapped.hEvent = Event.get();
}

WriteHandle::~WriteHandle()
{
    if (State == IOHandleStatus::Pending)
    {
        DWORD bytesRead{};
        LOG_IF_WIN32_BOOL_FALSE(CancelIoEx(Handle.get(), &Overlapped));
        LOG_IF_WIN32_BOOL_FALSE(GetOverlappedResult(Handle.get(), &Overlapped, &bytesRead, true));
    }
}

void WriteHandle::Schedule()
{
    WI_ASSERT(State == IOHandleStatus::Standby);

    Event.ResetEvent();

    // Schedule the write.
    DWORD bytesWritten{};
    if (WriteFile(Handle.get(), Buffer.data() + Offset, static_cast<DWORD>(Buffer.size() - Offset), &bytesWritten, &Overlapped))
    {
        Offset += bytesWritten;
        if (Offset >= Buffer.size())
        {
            State = IOHandleStatus::Completed;
        }
    }
    else
    {
        auto error = GetLastError();
        THROW_LAST_ERROR_IF_MSG(error != ERROR_IO_PENDING, "Handle: 0x%p", (void*)Handle.get());

        // The write is pending, update to 'Pending'
        State = IOHandleStatus::Pending;
    }
}

void WriteHandle::Collect()
{
    WI_ASSERT(State == IOHandleStatus::Pending);

    // Transition back to standby
    State = IOHandleStatus::Standby;

    // Complete the write.
    DWORD bytesWritten{};
    THROW_IF_WIN32_BOOL_FALSE(GetOverlappedResult(Handle.get(), &Overlapped, &bytesWritten, false));

    Offset += bytesWritten;
    if (Offset >= Buffer.size())
    {
        State = IOHandleStatus::Completed;
    }
}

HANDLE WriteHandle::GetHandle() const
{
    return Event.get();
}