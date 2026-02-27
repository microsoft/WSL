/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    svccomm.cpp

Abstract:

    This file contains function definitions for the SvcComm helper class.

--*/

#include "precomp.h"
#include "svccomm.hpp"
#include "registry.hpp"
#include "relay.hpp"

#pragma hdrstop

//
// Macros to test exit status (defined in sys\wait.h).
//

#define LXSS_WEXITSTATUS(_status) ((_status) >> 8)
#define LXSS_WSTATUS(_status) ((_status) & 0x7F)
#define LXSS_WIFEXITED(_status) (LXSS_WSTATUS((_status)) == 0)

#define IS_VALID_HANDLE(_handle) ((_handle != NULL) && (_handle != INVALID_HANDLE_VALUE))

#define TTY_ALT_NUMPAD_VK_MENU (0x12)
#define TTY_ESCAPE_CHARACTER (L'\x1b')
#define TTY_INPUT_EVENT_BUFFER_SIZE (16)
#define TTY_UTF8_TRANSLATION_BUFFER_SIZE (4 * TTY_INPUT_EVENT_BUFFER_SIZE)

using wsl::windows::common::ClientExecutionContext;
namespace {

BOOL GetNextCharacter(_In_ INPUT_RECORD* InputRecord, _Out_ PWCHAR NextCharacter);
BOOL IsActionableKey(_In_ PKEY_EVENT_RECORD KeyEvent);
void SpawnWslHost(_In_ HANDLE ServerPort, _In_ const GUID& DistroId, _In_opt_ LPCGUID VmId);

struct CreateProcessArguments
{
    CreateProcessArguments(LPCWSTR Filename, int Argc, LPCWSTR Argv[], ULONG LaunchFlags, LPCWSTR WorkingDirectory)
    {
        // Populate the current working directory.
        //
        // N.B. Failure to get the current working directory is non-fatal.
        if (ARGUMENT_PRESENT(WorkingDirectory))
        {
            // If a current working directory was provided, it must be a Linux-style path.
            WI_ASSERT(*WorkingDirectory == L'/' || *WorkingDirectory == L'~');

            CurrentWorkingDirectory = WorkingDirectory;
        }
        else
        {
            LOG_IF_FAILED(wil::GetCurrentDirectoryW(CurrentWorkingDirectory));
        }

        // Populate the command line and file name.
        //
        // N.B. The CommandLineStrings vector contains weak references to the
        //      strings in the CommandLine vector.
        if (Argc > 0)
        {
            CommandLine.reserve(Argc);
            std::transform(Argv, Argv + Argc, std::back_inserter(CommandLine), [](LPCWSTR Arg) {
                return wsl::shared::string::WideToMultiByte(Arg);
            });

            CommandLineStrings.reserve(CommandLine.size());
            std::transform(CommandLine.cbegin(), CommandLine.cend(), std::back_inserter(CommandLineStrings), [](const std::string& string) {
                return string.c_str();
            });
        }

        if (ARGUMENT_PRESENT(Filename))
        {
            FilenameString = wsl::shared::string::WideToMultiByte(Filename);
        }

        // Query the current NT %PATH% environment variable.
        //
        // N.B. Failure to query the path is non-fatal.
        LOG_IF_FAILED(wil::ExpandEnvironmentStringsW(L"%PATH%", NtPath));

        if (WI_IsFlagSet(LaunchFlags, LXSS_LAUNCH_FLAG_TRANSLATE_ENVIRONMENT))
        {
            NtEnvironment.reset(GetEnvironmentStringsW());

            // Calculate the size of the environment block.
            for (PCWSTR Variable = NtEnvironment.get(); Variable[0] != '\0';)
            {
                const size_t Length = wcslen(Variable) + 1;
                NtEnvironmentLength += Length;
                Variable += Length;
            }

            NtEnvironmentLength += 1;
        }
    }

    std::vector<std::string> CommandLine{};
    std::vector<LPCSTR> CommandLineStrings{};
    std::wstring CurrentWorkingDirectory{};
    std::string FilenameString{};
    wsl::windows::common::helpers::unique_environment_strings NtEnvironment;
    size_t NtEnvironmentLength{};
    std::wstring NtPath{};
};

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

void InitializeInterop(_In_ HANDLE ServerPort, _In_ const GUID& DistroId)
{
    //
    // Create a thread to handle interop requests.
    //

    wil::unique_handle WorkerThreadSeverPort{wsl::windows::common::wslutil::DuplicateHandle(ServerPort)};
    std::thread([WorkerThreadSeverPort = std::move(WorkerThreadSeverPort)]() mutable {
        wsl::windows::common::wslutil::SetThreadDescription(L"Interop");
        wsl::windows::common::interop::WorkerThread(std::move(WorkerThreadSeverPort));
    }).detach();

    //
    // Spawn wslhost to handle interop requests from processes that have
    // been backgrounded and their console window has been closed.
    //

    SpawnWslHost(ServerPort, DistroId, nullptr);
}

void SpawnWslHost(_In_ HANDLE ServerPort, _In_ const GUID& DistroId, _In_opt_ LPCGUID VmId)
{
    wsl::windows::common::helpers::SetHandleInheritable(ServerPort);
    const auto RegistrationComplete = wil::unique_event(wil::EventOptions::None);
    const wil::unique_handle ParentProcess{wsl::windows::common::wslutil::DuplicateHandle(GetCurrentProcess(), std::nullopt, TRUE)};
    THROW_LAST_ERROR_IF(!ParentProcess);

    const wil::unique_handle Process{wsl::windows::common::helpers::LaunchInteropServer(
        &DistroId, ServerPort, RegistrationComplete.get(), ParentProcess.get(), VmId)};

    // Wait for either the child to exit, or the registration complete event to be set.
    const HANDLE WaitHandles[] = {Process.get(), RegistrationComplete.get()};
    const DWORD WaitStatus = WaitForMultipleObjects(RTL_NUMBER_OF(WaitHandles), WaitHandles, FALSE, INFINITE);
    LOG_HR_IF_MSG(E_FAIL, (WaitStatus == WAIT_OBJECT_0), "wslhost failed to register");
}
} // namespace

//
// Exported function definitions.
//

void wsl::windows::common::RelayStandardInput(
    HANDLE ConsoleHandle,
    HANDLE OutputHandle,
    const std::shared_ptr<wsl::shared::SocketChannel>& ControlChannel,
    HANDLE ExitEvent,
    wsl::windows::common::ConsoleState* Io)
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
        LX_INIT_WINDOW_SIZE_CHANGED WindowSizeMessage{};
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
                if (ControlChannel)
                {
                    WindowSize = Io->GetWindowSize();
                    WindowSizeMessage.Header.MessageType = LxInitMessageWindowSizeChanged;
                    WindowSizeMessage.Header.MessageSize = sizeof(WindowSizeMessage);
                    WindowSizeMessage.Columns = WindowSize.X;
                    WindowSizeMessage.Rows = WindowSize.Y;

                    try
                    {
                        ControlChannel->SendMessage(WindowSizeMessage);
                    }
                    CATCH_LOG();
                }

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

wsl::windows::common::SvcComm::SvcComm()
{
    // Ensure that the OS has support for running lifted WSL. This interface is always present on Windows 11 and later.
    //
    // Prior to Windows 11 there are two cases where the IWslSupport interface may not be present:
    //     1. The machine has not installed the DCR that contains support for lifted WSL.
    //     2. The WSL optional component which contains the interface is not installed.
    if (!wsl::windows::common::helpers::IsWindows11OrAbove() && !wsl::windows::common::helpers::IsWslSupportInterfacePresent())
    {
        THROW_HR(wsl::windows::common::helpers::IsWslOptionalComponentPresent() ? WSL_E_OS_NOT_SUPPORTED : WSL_E_WSL_OPTIONAL_COMPONENT_REQUIRED);
    }

    auto retry_pred = []() {
        const auto errorCode = wil::ResultFromCaughtException();

        return errorCode == HRESULT_FROM_WIN32(ERROR_SERVICE_DOES_NOT_EXIST) || errorCode == REGDB_E_CLASSNOTREG;
    };

    wsl::shared::retry::RetryWithTimeout<void>(
        [this]() { m_userSession = wil::CoCreateInstance<LxssUserSession, ILxssUserSession>(CLSCTX_LOCAL_SERVER); },
        std::chrono::seconds(1),
        std::chrono::minutes(1),
        retry_pred);

    // Query client security interface.
    auto clientSecurity = m_userSession.query<IClientSecurity>();

    // Get the current proxy blanket settings.
    DWORD authnSvc, authzSvc, authnLvl, capabilities;
    THROW_IF_FAILED(clientSecurity->QueryBlanket(m_userSession.get(), &authnSvc, &authzSvc, NULL, &authnLvl, NULL, NULL, &capabilities));

    // Make sure that dynamic cloaking is used.
    WI_ClearFlag(capabilities, EOAC_STATIC_CLOAKING);
    WI_SetFlag(capabilities, EOAC_DYNAMIC_CLOAKING);
    THROW_IF_FAILED(clientSecurity->SetBlanket(
        m_userSession.get(), authnSvc, authzSvc, NULL, authnLvl, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, capabilities));
}

wsl::windows::common::SvcComm::~SvcComm()
{
}

void wsl::windows::common::SvcComm::ConfigureDistribution(_In_opt_ LPCGUID DistroGuid, _In_ ULONG DefaultUid, _In_ ULONG Flags) const
{
    ClientExecutionContext context;
    THROW_IF_FAILED(m_userSession->ConfigureDistribution(DistroGuid, DefaultUid, Flags, context.OutError()));
}

void wsl::windows::common::SvcComm::CreateInstance(_In_opt_ LPCGUID DistroGuid, _In_ ULONG Flags)
{
    ClientExecutionContext context;
    THROW_IF_FAILED(CreateInstanceNoThrow(DistroGuid, Flags, context.OutError()));
}

HRESULT
wsl::windows::common::SvcComm::CreateInstanceNoThrow(_In_opt_ LPCGUID DistroGuid, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error) const
{
    return m_userSession->CreateInstance(DistroGuid, Flags, Error);
}

std::vector<LXSS_ENUMERATE_INFO> wsl::windows::common::SvcComm::EnumerateDistributions() const
{
    ExecutionContext enumerateDistroContext(Context::EnumerateDistros);
    ClientExecutionContext context;

    wil::unique_cotaskmem_array_ptr<LXSS_ENUMERATE_INFO> Distributions;
    THROW_IF_FAILED(m_userSession->EnumerateDistributions(Distributions.size_address<ULONG>(), &Distributions, context.OutError()));

    std::vector<LXSS_ENUMERATE_INFO> DistributionList;
    for (size_t Index = 0; Index < Distributions.size(); Index += 1)
    {
        DistributionList.push_back(Distributions[Index]);
    }

    return DistributionList;
}

HRESULT
wsl::windows::common::SvcComm::ExportDistribution(_In_opt_ LPCGUID DistroGuid, _In_ HANDLE FileHandle, _In_ ULONG Flags) const
{
    ClientExecutionContext context;

    // Create a pipe for reading errors from bsdtar.
    wil::unique_handle stdErrRead;
    wil::unique_handle stdErrWrite;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&stdErrRead, &stdErrWrite, nullptr, 0));

    relay::ScopedRelay stdErrRelay(
        std::move(stdErrRead), GetStdHandle(STD_ERROR_HANDLE), LX_RELAY_BUFFER_SIZE, [&stdErrWrite]() { stdErrWrite.reset(); });

    HRESULT result = E_FAIL;
    if (GetFileType(FileHandle) != FILE_TYPE_PIPE)
    {
        result = m_userSession->ExportDistribution(DistroGuid, FileHandle, stdErrWrite.get(), Flags, context.OutError());
    }
    else
    {
        result = m_userSession->ExportDistributionPipe(DistroGuid, FileHandle, stdErrWrite.get(), Flags, context.OutError());
    }

    stdErrWrite.reset();
    stdErrRelay.Sync();

    RETURN_HR(result);
}

void wsl::windows::common::SvcComm::GetDistributionConfiguration(
    _In_opt_ LPCGUID DistroGuid,
    _Out_ LPWSTR* Name,
    _Out_ ULONG* Version,
    _Out_ ULONG* DefaultUid,
    _Out_ ULONG* DefaultEnvironmentCount,
    _Out_ LPSTR** DefaultEnvironment,
    _Out_ ULONG* Flags) const
{
    ClientExecutionContext context;

    THROW_IF_FAILED(m_userSession->GetDistributionConfiguration(
        DistroGuid, Name, Version, DefaultUid, DefaultEnvironmentCount, DefaultEnvironment, Flags, context.OutError()));
}

DWORD
wsl::windows::common::SvcComm::LaunchProcess(
    _In_opt_ LPCGUID DistroGuid,
    _In_opt_ LPCWSTR Filename,
    _In_ int Argc,
    _In_reads_(Argc) LPCWSTR Argv[],
    _In_ ULONG LaunchFlags,
    _In_opt_ PCWSTR Username,
    _In_opt_ PCWSTR CurrentWorkingDirectory,
    _In_ DWORD Timeout) const
{
    ClientExecutionContext context;

    //
    // Parse the input arguments.
    //

    DWORD ExitCode = 1;
    CreateProcessArguments Parsed(Filename, Argc, Argv, LaunchFlags, CurrentWorkingDirectory);

    //
    // Create the process.
    //

    ConsoleState Io;
    COORD WindowSize = Io.GetWindowSize();
    ULONG Flags = LXSS_CREATE_INSTANCE_FLAGS_ALLOW_FS_UPGRADE;
    if (WI_IsFlagSet(LaunchFlags, LXSS_LAUNCH_FLAG_USE_SYSTEM_DISTRO))
    {
        WI_SetFlag(Flags, LXSS_CREATE_INSTANCE_FLAGS_USE_SYSTEM_DISTRO);
    }

    if (WI_IsFlagSet(LaunchFlags, LXSS_LAUNCH_FLAG_SHELL_LOGIN))
    {
        WI_SetFlag(Flags, LXSS_CREATE_INSTANCE_FLAGS_SHELL_LOGIN);
    }

    // This method is also used by Terminal.
    // See: https://github.com/microsoft/terminal/blob/ec434e3fba2a6ef254123e31f5257c25b04f2547/src/tools/ConsoleBench/conhost.cpp#L159-L164
    HANDLE console = NtCurrentTeb()->ProcessEnvironmentBlock->ProcessParameters->Reserved2[0];

    LXSS_STD_HANDLES StdHandles{};
    const HANDLE InputHandle = GetStdHandle(STD_INPUT_HANDLE);
    const bool IsConsoleInput = wsl::windows::common::wslutil::IsConsoleHandle(InputHandle);
    StdHandles.StdIn.HandleType = IsConsoleInput ? LxssHandleConsole : LxssHandleInput;
    StdHandles.StdIn.Handle = IsConsoleInput ? LXSS_HANDLE_USE_CONSOLE : HandleToUlong(InputHandle);
    const HANDLE OutputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool IsConsoleOutput = wsl::windows::common::wslutil::IsConsoleHandle(OutputHandle);
    StdHandles.StdOut.HandleType = IsConsoleOutput ? LxssHandleConsole : LxssHandleOutput;
    StdHandles.StdOut.Handle = IsConsoleOutput ? LXSS_HANDLE_USE_CONSOLE : HandleToUlong(OutputHandle);
    const HANDLE ErrorHandle = GetStdHandle(STD_ERROR_HANDLE);
    const bool IsConsoleError = wsl::windows::common::wslutil::IsConsoleHandle(ErrorHandle);
    StdHandles.StdErr.HandleType = IsConsoleError ? LxssHandleConsole : LxssHandleOutput;
    StdHandles.StdErr.Handle = IsConsoleError ? LXSS_HANDLE_USE_CONSOLE : HandleToUlong(ErrorHandle);

    GUID DistributionId;
    GUID InstanceId;
    wil::unique_handle ProcessHandle;
    wil::unique_handle ServerPortHandle;
    wil::unique_handle StdInSocket;
    wil::unique_handle StdOutSocket;
    wil::unique_handle StdErrSocket;
    wil::unique_handle ControlSocket;
    wil::unique_handle InteropSocket;

    if (GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_CHAR)
    {
        context.EnableInteractiveWarnings();
    }

    THROW_IF_FAILED(m_userSession->CreateLxProcess(
        DistroGuid,
        Parsed.FilenameString.empty() ? nullptr : Parsed.FilenameString.c_str(),
        Argc,
        Parsed.CommandLineStrings.data(),
        Parsed.CurrentWorkingDirectory.empty() ? nullptr : Parsed.CurrentWorkingDirectory.c_str(),
        Parsed.NtPath.empty() ? nullptr : Parsed.NtPath.c_str(),
        Parsed.NtEnvironment.get(),
        static_cast<ULONG>(Parsed.NtEnvironmentLength),
        Username,
        WindowSize.X,
        WindowSize.Y,
        HandleToUlong(console),
        &StdHandles,
        Flags,
        &DistributionId,
        &InstanceId,
        &ProcessHandle,
        &ServerPortHandle,
        &StdInSocket,
        &StdOutSocket,
        &StdErrSocket,
        &ControlSocket,
        &InteropSocket,
        context.OutError()));

    context.FlushWarnings();

    WI_ASSERT((!ARGUMENT_PRESENT(DistroGuid)) || (IsEqualGUID(*DistroGuid, DistributionId)));

    //
    // If a process handle was returned, this is a WSL process. Otherwise, the
    // process is running in a utility VM.
    //

    if (ProcessHandle)
    {
        //
        // Mark the process handle as uninheritable.
        //

        helpers::SetHandleInheritable(ProcessHandle.get(), false);

        //
        // If the caller requested interop and a server port was created, start
        // the interop worker thread and background wslhost process.
        //

        if ((WI_IsFlagSet(LaunchFlags, LXSS_LAUNCH_FLAG_ENABLE_INTEROP)) && (ServerPortHandle))
        {
            try
            {
                InitializeInterop(ServerPortHandle.get(), DistributionId);
            }
            CATCH_LOG()
        }

        ServerPortHandle.reset();

        //
        // Wait for the launched process to exit and return the process exit
        // code.
        //

        LXBUS_IPC_LX_PROCESS_WAIT_FOR_TERMINATION_PARAMETERS Parameters{};
        Parameters.Input.TimeoutMs = Timeout;
        THROW_IF_NTSTATUS_FAILED(LxBusClientWaitForLxProcess(ProcessHandle.get(), &Parameters));

        if (LXSS_WIFEXITED(Parameters.Output.ExitStatus))
        {
            Parameters.Output.ExitStatus = LXSS_WEXITSTATUS(Parameters.Output.ExitStatus);
        }

        ExitCode = Parameters.Output.ExitStatus;
    }
    else
    {
        //
        // Create stdin, stdout and stderr worker threads.
        //

        std::thread StdOutWorker;
        std::thread StdErrWorker;
        auto ExitEvent = wil::unique_event(wil::EventOptions::ManualReset);
        auto outWorkerExit = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&StdOutWorker, &StdErrWorker, &ExitEvent] {
            ExitEvent.SetEvent();
            if (StdOutWorker.joinable())
            {
                StdOutWorker.join();
            }

            if (StdErrWorker.joinable())
            {
                StdErrWorker.join();
            }
        });

        // This channel needs to be a shared_ptr because closing it will cause the linux relay to exit so we should keep it open
        // even after the stdin thread exits, but we can't keep give a simple reference to that thread because the main thread
        // might return from this method before the stdin relay thread does.

        auto ControlChannel = std::make_shared<wsl::shared::SocketChannel>(
            wil::unique_socket{reinterpret_cast<SOCKET>(ControlSocket.release())}, "Control");

        auto StdIn = GetStdHandle(STD_INPUT_HANDLE);
        if (IS_VALID_HANDLE(StdIn))
        {
            std::thread([StdIn, StdInSocket = std::move(StdInSocket), ControlChannel = ControlChannel, ExitHandle = ExitEvent.get(), Io = &Io]() mutable {
                RelayStandardInput(StdIn, StdInSocket.get(), ControlChannel, ExitHandle, Io);
            }).detach();
        }

        auto StdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        StdOutWorker = relay::CreateThread(std::move(StdOutSocket), IS_VALID_HANDLE(StdOut) ? StdOut : nullptr);
        auto StdErr = GetStdHandle(STD_ERROR_HANDLE);
        StdErrWorker = relay::CreateThread(std::move(StdErrSocket), IS_VALID_HANDLE(StdErr) ? StdErr : nullptr);

        //
        // Spawn wslhost to handle interop requests from processes that have
        // been backgrounded and their console window has been closed.
        //

        if (WI_IsFlagSet(LaunchFlags, LXSS_LAUNCH_FLAG_ENABLE_INTEROP))
        {
            try
            {
                SpawnWslHost(InteropSocket.get(), DistributionId, &InstanceId);
            }
            CATCH_LOG()
        }

        //
        // Begin reading messages from the utility vm.
        //

        wsl::shared::SocketChannel InteropChannel{
            wil::unique_socket{reinterpret_cast<SOCKET>(InteropSocket.release())}, "Interop"};
        ExitCode = interop::VmModeWorkerThread(InteropChannel, InstanceId);
    }

    return ExitCode;
}

GUID wsl::windows::common::SvcComm::GetDefaultDistribution() const
{
    ClientExecutionContext context;
    GUID DistroId;
    THROW_IF_FAILED(m_userSession->GetDefaultDistribution(context.OutError(), &DistroId));

    return DistroId;
}

ULONG
wsl::windows::common::SvcComm::GetDistributionFlags(_In_opt_ LPCGUID DistroGuid) const
{
    ClientExecutionContext context;

    wil::unique_cotaskmem_string Name;
    ULONG Version;
    ULONG Uid;
    wil::unique_cotaskmem_array_ptr<wil::unique_cotaskmem_ansistring> Environment;
    ULONG Flags;
    THROW_IF_FAILED(m_userSession->GetDistributionConfiguration(
        DistroGuid, &Name, &Version, &Uid, Environment.size_address<ULONG>(), &Environment, &Flags, context.OutError()));

    return Flags;
}

GUID wsl::windows::common::SvcComm::GetDistributionId(_In_ LPCWSTR Name, _In_ ULONG Flags) const
{
    ClientExecutionContext context;

    GUID DistroId;
    THROW_IF_FAILED(m_userSession->GetDistributionId(Name, Flags, context.OutError(), &DistroId));

    return DistroId;
}

GUID wsl::windows::common::SvcComm::ImportDistributionInplace(_In_ LPCWSTR Name, _In_ LPCWSTR VhdPath) const
{
    ClientExecutionContext context;

    GUID DistroGuid;
    THROW_IF_FAILED(m_userSession->ImportDistributionInplace(Name, VhdPath, context.OutError(), &DistroGuid));

    return DistroGuid;
}

void wsl::windows::common::SvcComm::MoveDistribution(_In_ const GUID& DistroGuid, _In_ LPCWSTR Location) const
{
    ClientExecutionContext context;

    THROW_IF_FAILED(m_userSession->MoveDistribution(&DistroGuid, Location, context.OutError()));
}

std::pair<GUID, wil::unique_cotaskmem_string> wsl::windows::common::SvcComm::RegisterDistribution(
    _In_ LPCWSTR Name,
    _In_ ULONG Version,
    _In_ HANDLE FileHandle,
    _In_ LPCWSTR TargetDirectory,
    _In_ ULONG Flags,
    _In_ std::optional<uint64_t> VhdSize,
    _In_opt_ LPCWSTR PackageFamilyName) const
{
    ClientExecutionContext context;

    // Create a pipe for reading errors from bsdtar.
    wil::unique_handle stdErrRead;
    wil::unique_handle stdErrWrite;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&stdErrRead, &stdErrWrite, nullptr, 0));

    relay::ScopedRelay stdErrRelay(
        std::move(stdErrRead), GetStdHandle(STD_ERROR_HANDLE), LX_RELAY_BUFFER_SIZE, [&stdErrWrite]() { stdErrWrite.reset(); });

    GUID DistroGuid{};
    HRESULT Result = E_FAIL;
    wil::unique_cotaskmem_string installedName;
    if (GetFileType(FileHandle) != FILE_TYPE_PIPE)
    {
        Result = m_userSession->RegisterDistribution(
            Name,
            Version,
            FileHandle,
            stdErrWrite.get(),
            TargetDirectory,
            Flags,
            VhdSize.value_or(0),
            PackageFamilyName,
            &installedName,
            context.OutError(),
            &DistroGuid);
    }
    else
    {
        Result = m_userSession->RegisterDistributionPipe(
            Name,
            Version,
            FileHandle,
            stdErrWrite.get(),
            TargetDirectory,
            Flags,
            VhdSize.value_or(0),
            PackageFamilyName,
            &installedName,
            context.OutError(),
            &DistroGuid);
    }

    stdErrWrite.reset();
    stdErrRelay.Sync();

    THROW_IF_FAILED(Result);

    return std::make_pair(DistroGuid, std::move(installedName));
}

void wsl::windows::common::SvcComm::SetDefaultDistribution(_In_ LPCGUID DistroGuid) const
{
    ClientExecutionContext context;
    THROW_IF_FAILED(m_userSession->SetDefaultDistribution(DistroGuid, context.OutError()));
}

HRESULT
wsl::windows::common::SvcComm::SetSparse(_In_ LPCGUID DistroGuid, _In_ BOOL Sparse, _In_ BOOL AllowUnsafe) const
{
    ClientExecutionContext context;

    RETURN_HR(m_userSession->SetSparse(DistroGuid, Sparse, AllowUnsafe, context.OutError()));
}

HRESULT
wsl::windows::common::SvcComm::ResizeDistribution(_In_ LPCGUID DistroGuid, _In_ ULONG64 NewSize) const
{
    ClientExecutionContext context;

    wil::unique_handle outputRead;
    wil::unique_handle outputWrite;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&outputRead, &outputWrite, nullptr, 0));

    relay::ScopedRelay outputRelay(
        std::move(outputRead), GetStdHandle(STD_ERROR_HANDLE), LX_RELAY_BUFFER_SIZE, [&outputWrite]() { outputWrite.reset(); });

    const auto result = m_userSession->ResizeDistribution(DistroGuid, outputWrite.get(), NewSize, context.OutError());

    outputWrite.reset();
    outputRelay.Sync();

    RETURN_HR(result);
}

HRESULT
wsl::windows::common::SvcComm::SetVersion(_In_ LPCGUID DistroGuid, _In_ ULONG Version) const
{
    ClientExecutionContext context;

    // Create a pipe for reading errors from bsdtar.
    wil::unique_handle stdErrRead;
    wil::unique_handle stdErrWrite;
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&stdErrRead, &stdErrWrite, nullptr, 0));

    relay::ScopedRelay stdErrRelay(
        std::move(stdErrRead), GetStdHandle(STD_ERROR_HANDLE), LX_RELAY_BUFFER_SIZE, [&stdErrWrite]() { stdErrWrite.reset(); });

    RETURN_HR(m_userSession->SetVersion(DistroGuid, Version, stdErrWrite.get(), context.OutError()));
}

HRESULT
wsl::windows::common::SvcComm::AttachDisk(_In_ LPCWSTR Disk, _In_ ULONG Flags) const
{
    ClientExecutionContext context;

    RETURN_HR(m_userSession->AttachDisk(Disk, Flags, context.OutError()));
}

std::pair<int, int> wsl::windows::common::SvcComm::DetachDisk(_In_opt_ LPCWSTR Disk) const
{
    ClientExecutionContext context;

    int Result = -1;
    int Step = 0;
    THROW_IF_FAILED(m_userSession->DetachDisk(Disk, &Result, &Step, context.OutError()));

    return std::make_pair(Result, Step);
}

wsl::windows::common::SvcComm::MountResult wsl::windows::common::SvcComm::MountDisk(
    _In_ LPCWSTR Disk, _In_ ULONG Flags, _In_ ULONG PartitionIndex, _In_opt_ LPCWSTR Name, _In_opt_ LPCWSTR Type, _In_opt_ LPCWSTR Options) const
{
    ClientExecutionContext context;

    MountResult Result;
    THROW_IF_FAILED(m_userSession->MountDisk(
        Disk, Flags, PartitionIndex, Name, Type, Options, &Result.Result, &Result.Step, &Result.MountName, context.OutError()));

    return Result;
}

void wsl::windows::common::SvcComm::Shutdown(_In_ bool Force) const
{
    THROW_IF_FAILED(m_userSession->Shutdown(Force));
}

void wsl::windows::common::SvcComm::TerminateInstance(_In_opt_ LPCGUID DistroGuid) const
{
    ClientExecutionContext context;

    //
    // If there is an instance running, terminate it.
    //

    THROW_IF_FAILED(m_userSession->TerminateDistribution(DistroGuid, context.OutError()));
}

void wsl::windows::common::SvcComm::UnregisterDistribution(_In_ LPCGUID DistroGuid) const
{
    ClientExecutionContext context;
    THROW_IF_FAILED(m_userSession->UnregisterDistribution(DistroGuid, context.OutError()));
}
