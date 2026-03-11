/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    interop.cpp

Abstract:

    This file contains interop function definitions.

--*/

#include "precomp.h"
#include "interop.hpp"
#include "helpers.hpp"
#include "socket.hpp"
#include "hvsocket.hpp"
#include "relay.hpp"
#include "LxssServerPort.h"
#include "LxssMessagePort.h"
#include <gsl/algorithm>
#include <gslhelpers.h>
#include "WslTelemetry.h"

namespace {

std::wstring BuildEnvironment(gsl::span<gsl::byte> EnvironmentData);

std::string FormatCommandLine(gsl::span<gsl::byte> CommandLineData, USHORT CommandLineCount);

struct CreateProcessResult;
DWORD ProcessInteropMessages(_In_ HANDLE CommunicationChannel, _Inout_ CreateProcessResult* Result);

struct CreateProcessParsed
{
    CreateProcessParsed(_In_ const gsl::span<gsl::byte>& Common)
    {
        // Validate the message size and get spans to the various buffers. Note
        // that the spans will be larger than the actual data since the message
        // does not specify the size of each data element; the length is encoded
        // via NULL termination.

        const auto* Params = gslhelpers::try_get_struct<LX_INIT_CREATE_NT_PROCESS_COMMON>(Common);
        THROW_HR_IF(E_INVALIDARG, !Params || (Common.size() < (Params->CommandLineOffset)));

        // Parse the application name, command line, and current working directory
        // and convert to unicode.

        ApplicationName = wsl::shared::string::MultiByteToWide(wsl::shared::string::FromSpan(Common, Params->FilenameOffset));
        const auto FormattedCommandLine = FormatCommandLine(Common.subspan(Params->CommandLineOffset), Params->CommandLineCount);
        CommandLineBuffer = wsl::shared::string::MultiByteToWide(FormattedCommandLine);
        const auto* Cwd = wsl::shared::string::FromSpan(Common, Params->CurrentWorkingDirectoryOffset);
        if (strlen(Cwd) > 0)
        {
            CwdBuffer = wsl::shared::string::MultiByteToWide(Cwd);
        }

        // Construct an environment if one was provided.

        if (Params->EnvironmentOffset > 0)
        {
            THROW_HR_IF(E_INVALIDARG, Common.size() < Params->EnvironmentOffset);

            EnvironmentBuffer = BuildEnvironment(Common.subspan(Params->EnvironmentOffset));
        }

        Rows = Params->Rows;
        Columns = Params->Columns;
        CreatePseudoconsole = Params->CreatePseudoconsole;
    }

    LPWSTR CommandLine()
    {
        return CommandLineBuffer.data();
    }

    LPCWSTR Cwd() const
    {
        return CwdBuffer.empty() ? nullptr : CwdBuffer.c_str();
    }

    LPVOID Environment()
    {
        return EnvironmentBuffer.empty() ? nullptr : EnvironmentBuffer.data();
    }

    std::wstring ApplicationName{};
    std::wstring CommandLineBuffer{};
    std::wstring EnvironmentBuffer{};
    std::wstring CwdBuffer{};
    DWORD Rows{};
    DWORD Columns{};
    bool CreatePseudoconsole{};
};

struct CreateProcessResult
{
    wil::unique_handle Process{};
    int Status{};
    unsigned int Flags{};
    wsl::windows::common::helpers::unique_pseudo_console PseudoConsole{};
};

struct CreateProcessVmModeContext
{
    GUID VmId{};
    std::vector<gsl::byte> Buffer{};
};

std::wstring BuildEnvironment(gsl::span<gsl::byte> EnvironmentData)
{
    std::map<std::wstring, std::wstring> Environment;

    // Construct a map of the current environment strings.
    const wsl::windows::common::helpers::unique_environment_strings EnvironmentStrings(GetEnvironmentStrings());
    PCZZWSTR CurrentEnvironment = EnvironmentStrings.get();
    while (*CurrentEnvironment)
    {
        const PCWSTR Divider = wcschr(CurrentEnvironment, '=');
        THROW_HR_IF_NULL(E_UNEXPECTED, Divider);
        std::wstring Key(CurrentEnvironment, Divider);
        Environment[Key] = Divider + 1;
        CurrentEnvironment += wcslen(CurrentEnvironment) + 1;
    }

    // Update the map with the Linux environment data.
    for (;;)
    {
        std::string_view Variable = wsl::shared::string::FromSpan(EnvironmentData);
        if (Variable.empty())
        {
            break;
        }

        const size_t Divider = Variable.find('=');
        THROW_HR_IF(E_UNEXPECTED, Divider == Variable.npos);
        std::wstring Key = wsl::shared::string::MultiByteToWide(std::string{Variable.substr(0, Divider)});
        auto Value = Variable.substr(Divider + 1);
        if (Value.empty())
        {
            Environment.erase(Key);
        }
        else
        {
            Environment[Key] = wsl::shared::string::MultiByteToWide(std::string{Value});
        }

        EnvironmentData = EnvironmentData.subspan(Variable.size() + 1);
    }

    // Construct a new environment block.
    std::wstring Block;
    for (const auto& Variable : Environment)
    {
        Block += Variable.first;
        Block += L'=';
        Block += Variable.second;
        Block += L'\0';
    }

    return Block;
}

HRESULT GetProcessImageSubSystem(_In_ HANDLE Process, _Out_ ULONG* ImageSubsystem)
{
    *ImageSubsystem = IMAGE_SUBSYSTEM_UNKNOWN;

    PROCESS_BASIC_INFORMATION ProcessBasicInfo{};
    RETURN_IF_NTSTATUS_FAILED(NtQueryInformationProcess(Process, ProcessBasicInformation, &ProcessBasicInfo, sizeof(ProcessBasicInfo), NULL));

    // Terminal uses a similar method to read the PEB.
    // See: https://github.com/microsoft/terminal/blob/ec434e3fba2a6ef254123e31f5257c25b04f2547/src/tools/ConsoleBench/conhost.cpp#L159-L164
    PEB* dummyPeb = nullptr;
    const auto offset = ((char*)&dummyPeb->Reserved9[24]) - (char*)dummyPeb;

    SIZE_T BytesRead = 0;
    RETURN_IF_WIN32_BOOL_FALSE(ReadProcessMemory(
        Process, ((char*)ProcessBasicInfo.PebBaseAddress) + offset, ImageSubsystem, sizeof(*ImageSubsystem), &BytesRead));

    if (BytesRead < sizeof(*ImageSubsystem))
    {
        *ImageSubsystem = IMAGE_SUBSYSTEM_UNKNOWN;
        return E_UNEXPECTED;
    }

    return S_OK;
}

CreateProcessResult CreateProcess(_In_ CreateProcessParsed* Parsed, _In_ HANDLE StdIn, _In_ HANDLE StdOut, _In_ HANDLE StdErr)
{
    wsl::windows::common::helpers::SetHandleInheritable(StdIn);
    wsl::windows::common::helpers::SetHandleInheritable(StdOut);
    wsl::windows::common::helpers::SetHandleInheritable(StdErr);

    // N.B. Passing StartupFlags = 0 so that the cursor feedback is set to its default behavior.
    // See: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-startupinfoa
    wsl::windows::common::SubProcess process(Parsed->ApplicationName.c_str(), Parsed->CommandLine(), CREATE_UNICODE_ENVIRONMENT, 0);

    CreateProcessResult Result{};
    if (Parsed->CreatePseudoconsole)
    {
        const COORD Size{static_cast<SHORT>(Parsed->Columns), static_cast<SHORT>(Parsed->Rows)};
        THROW_IF_FAILED(CreatePseudoConsole(Size, StdIn, StdOut, PSEUDOCONSOLE_INHERIT_CURSOR, &Result.PseudoConsole));

        process.SetPseudoConsole(Result.PseudoConsole.get());
    }
    else
    {
        // In the case where this is a console process, don't create a new console window.
        // This is useful for wslg.exe, when a console program is created through interop,
        // we don't want to create a new console window.
        // N.B. CREATE_NO_WINDOW only applies to console executables, so GUI applications
        // are not affected by this flag.
        process.SetFlags(CREATE_NO_WINDOW);
        process.SetStdHandles(StdIn, StdOut, StdErr);
    }

    // Set the breakaway override flag to ensure that processes created via interop are not packaged.
    process.SetDesktopAppPolicy(PROCESS_CREATION_DESKTOP_APP_BREAKAWAY_OVERRIDE);
    process.SetEnvironment(Parsed->Environment());
    process.SetWorkingDirectory(Parsed->Cwd());

    try
    {
        Result.Process = process.Start();
        // Check if the process that was launched is a graphical application.
        // Non-graphical applications should be terminated when the file
        // descriptor that represents the process is closed.
        ULONG ImageSubsystem = IMAGE_SUBSYSTEM_UNKNOWN;
        LOG_IF_FAILED(GetProcessImageSubSystem(Result.Process.get(), &ImageSubsystem));
        const bool IsGuiApp = (ImageSubsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI);
        WI_SetFlagIf(Result.Flags, LX_INIT_CREATE_PROCESS_RESULT_FLAG_GUI_APPLICATION, IsGuiApp);
    }
    catch (...)
    {
        const DWORD LastError = wil::ResultFromCaughtException();
        switch (LastError)
        {
        case ERROR_FILE_NOT_FOUND:
            Result.Status = -LX_ENOENT;
            break;

        case ERROR_ELEVATION_REQUIRED:
            Result.Status = -LX_EACCES;
            break;

        default:
            Result.Status = -LX_EINVAL;
            LOG_IF_WIN32_ERROR_MSG(LastError, "CreateProcessW");
            break;
        }
    }

    return Result;
}

void CreateProcessVmMode(_In_ const GUID& VmId, _In_ const gsl::span<gsl::byte>& Buffer)
{
    // Create a worker thread to service the interop request.
    //
    // N.B. The worker thread takes ownership of the arguments.
    auto Arguments = std::make_unique<CreateProcessVmModeContext>();
    Arguments->VmId = VmId;
    Arguments->Buffer.resize(Buffer.size());
    gsl::copy(Buffer, gsl::make_span(Arguments->Buffer));
    std::thread([Arguments = std::move(Arguments)]() {
        try
        {
            wsl::windows::common::wslutil::SetThreadDescription(L"Interop");
            auto Message = gsl::make_span(Arguments->Buffer);
            auto* Params = gslhelpers::try_get_struct<LX_INIT_CREATE_NT_PROCESS_UTILITY_VM>(Message);
            THROW_HR_IF(E_INVALIDARG, !Params || (Params->Header.MessageType != LxInitMessageCreateProcessUtilityVm));

            // Parse the message.
            CreateProcessParsed Parsed(Message.subspan(offsetof(LX_INIT_CREATE_NT_PROCESS_UTILITY_VM, Common)));

            // Establish connections on the specified port.
            static_assert(LX_INIT_CREATE_NT_PROCESS_SOCKETS == 4);

            wil::unique_socket Sockets[LX_INIT_CREATE_NT_PROCESS_SOCKETS];
            for (ULONG Index = 0; Index < RTL_NUMBER_OF(Sockets); Index += 1)
            {
                Sockets[Index] = wsl::windows::common::hvsocket::Connect(Arguments->VmId, Params->Port);
            }

            // Clean up relay threads.
            //
            // N.B. This must be declared before the stdin / stdout / stderr handles so they go out of scope first.
            std::vector<std::thread> Relays;
            auto CancelIo = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&Relays] {
                for (auto& Relay : Relays)
                {
                    if (Relay.joinable())
                    {
                        Relay.join();
                    }
                }
            });

            // If a pseudoconsole is not being used, create hvsocket / pipe relays.
            wil::unique_handle StdIn{reinterpret_cast<HANDLE>(Sockets[0].release())};
            wil::unique_handle StdOut{reinterpret_cast<HANDLE>(Sockets[1].release())};
            wil::unique_handle StdErr{reinterpret_cast<HANDLE>(Sockets[2].release())};
            if (Parsed.CreatePseudoconsole == FALSE)
            {
                auto Pipe = wsl::windows::common::wslutil::OpenAnonymousPipe(0, false, true);
                Relays.push_back(wsl::windows::common::relay::CreateThread(std::move(StdIn), wil::unique_handle{Pipe.second.release()}));
                StdIn.reset(Pipe.first.release());

                Pipe = wsl::windows::common::wslutil::OpenAnonymousPipe(0, true, false);
                Relays.push_back(wsl::windows::common::relay::CreateThread(wil::unique_handle{Pipe.first.release()}, std::move(StdOut)));
                StdOut.reset(Pipe.second.release());

                Pipe = wsl::windows::common::wslutil::OpenAnonymousPipe(0, true, false);
                Relays.push_back(wsl::windows::common::relay::CreateThread(wil::unique_handle{Pipe.first.release()}, std::move(StdErr)));
                StdErr.reset(Pipe.second.release());
            }

            // Launch the process and write the status via the control channel.
            auto Result = CreateProcess(&Parsed, StdIn.get(), StdOut.get(), StdErr.get());
            LX_INIT_CREATE_PROCESS_RESPONSE Response{};
            Response.Header.MessageType = LxInitMessageCreateProcessResponse;
            Response.Header.MessageSize = sizeof(Response);
            Response.Flags = Result.Flags;
            Response.Result = Result.Status;
            wsl::windows::common::socket::Send(Sockets[3].get(), gslhelpers::struct_as_bytes(Response));
            if (Result.Status == 0)
            {
                // Process messages from the binfmt interpreter and wait for the process to exit.
                LX_INIT_PROCESS_EXIT_STATUS ExitStatus;
                ExitStatus.Header.MessageType = LxInitMessageExitStatus;
                ExitStatus.Header.MessageSize = sizeof(ExitStatus);
                ExitStatus.ExitCode = ProcessInteropMessages(reinterpret_cast<HANDLE>(Sockets[3].get()), &Result);

                // Write the exit status to the binfmt interpreter.
                wsl::windows::common::socket::Send(Sockets[3].get(), gslhelpers::struct_as_bytes(ExitStatus));
            }
        }
        CATCH_LOG()
    }).detach();
}

std::string FormatCommandLine(gsl::span<gsl::byte> CommandLineData, USHORT CommandLineCount)
{
    // Concatenate all of the command line arguments into a single string for
    // the CreateProcess api.
    //
    // N.B. Any empty arguments or arguments that contain whitespace must be
    //      encapsulated in quotes. Quotes must also be escaped according to
    //      standard command-line parsing rules:
    //      https://msdn.microsoft.com/en-us/library/17w5ykft.aspx.
    //
    //      This logic is largely taken from AppendQuotedForWindows in
    //      hcsdiag.cpp. In the future it would make sense to merge this
    //      functionality.
    std::string CommandLine;
    for (USHORT Index = 0; Index < CommandLineCount; Index += 1)
    {
        std::string_view Buffer = wsl::shared::string::FromSpan(CommandLineData);
        if (!Buffer.empty() && Buffer.find_first_of(" \t\r\n\"") == Buffer.npos)
        {
            CommandLine.append(Buffer);
        }
        else
        {
            CommandLine += '"';
            size_t BackslashCount = 0;
            for (const char Ch : Buffer)
            {
                switch (Ch)
                {
                case '"':
                    CommandLine.append(((BackslashCount * 2) + 1), '\\');
                    BackslashCount = 0;
                    CommandLine += '"';
                    break;

                case '\\':
                    BackslashCount += 1;
                    break;

                default:
                    CommandLine.append(BackslashCount, '\\');
                    BackslashCount = 0;
                    CommandLine += Ch;
                    break;
                }
            }

            CommandLine.append(BackslashCount * 2, '\\');
            CommandLine += '"';
        }

        // Add a space between command line arguments.
        if (Index < CommandLineCount - 1)
        {
            CommandLine += ' ';
        }

        CommandLineData = CommandLineData.subspan(Buffer.size() + 1);
    }

    return CommandLine;
}

DWORD
ProcessInteropMessages(_In_ HANDLE MessageHandle, _Inout_ CreateProcessResult* Result)
{
    OVERLAPPED Overlapped = {0};
    const wil::unique_event OverlappedEvent(wil::EventOptions::ManualReset);
    Overlapped.hEvent = OverlappedEvent.get();
    const HANDLE WaitHandles[] = {Overlapped.hEvent, Result->Process.get()};

    // Read messages from the message handle. Break out of the loop if the pipe
    // is connection is closed or the process exits.
    //
    // N.B. ReadFile will automatically reset the event in the overlapped
    //      structure.
    DWORD ExitCode = 1;
    for (;;)
    {
        DWORD BytesRead;
        LX_INIT_WINDOW_SIZE_CHANGED WindowSizeMessage;
        bool Success = ReadFile(MessageHandle, &WindowSizeMessage, sizeof(WindowSizeMessage), &BytesRead, &Overlapped);
        if (!Success)
        {
            const auto LastError = GetLastError();
            if ((LastError == ERROR_BROKEN_PIPE) || (LastError == ERROR_HANDLE_EOF))
            {
                if (WI_IsFlagClear(Result->Flags, LX_INIT_CREATE_PROCESS_RESULT_FLAG_GUI_APPLICATION))
                {
                    THROW_IF_WIN32_BOOL_FALSE(TerminateProcess(Result->Process.get(), 1));
                }

                break;
            }

            THROW_LAST_ERROR_IF(LastError != ERROR_IO_PENDING);

            auto CancelIo = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
                CancelIoEx(MessageHandle, &Overlapped);
                GetOverlappedResult(MessageHandle, &Overlapped, &BytesRead, TRUE);
            });

            const DWORD WaitStatus = WaitForMultipleObjects(RTL_NUMBER_OF(WaitHandles), WaitHandles, FALSE, INFINITE);
            if (WaitStatus == WAIT_OBJECT_0)
            {
                Success = GetOverlappedResult(MessageHandle, &Overlapped, &BytesRead, FALSE);
                CancelIo.release();
                if ((!Success) || (BytesRead == 0))
                {
                    if (WI_IsFlagClear(Result->Flags, LX_INIT_CREATE_PROCESS_RESULT_FLAG_GUI_APPLICATION))
                    {
                        THROW_IF_WIN32_BOOL_FALSE(TerminateProcess(Result->Process.get(), 1));
                    }

                    break;
                }

                WI_ASSERT((BytesRead == sizeof(WindowSizeMessage)) && (WindowSizeMessage.Header.MessageType == LxInitMessageWindowSizeChanged));

                const COORD Size{static_cast<SHORT>(WindowSizeMessage.Columns), static_cast<SHORT>(WindowSizeMessage.Rows)};
                THROW_IF_FAILED(ResizePseudoConsole(Result->PseudoConsole.get(), Size));
            }
            else if (WaitStatus == (WAIT_OBJECT_0 + 1))
            {
                THROW_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(Result->Process.get(), &ExitCode));

                // Close the pseudoconsole, this causes all pending data to be flushed.
                Result->PseudoConsole.reset();
                break;
            }
            else
            {
                THROW_HR(E_UNEXPECTED);
            }
        }
    }

    return ExitCode;
}

} // namespace

void wsl::windows::common::interop::WorkerThread(_In_ wil::unique_handle&& ServerPortHandle)
{
    // This thread waits for connections and processes create process messages.
    //
    // N.B. This thread lives until the main thread of the process exits.
    //
    // TODO_LX: Wait for connection blocks in the driver until the server port
    //          is closed. The wait for connection ioctl should be moved to
    //          async so this thread can wait on the server port and a second
    //          event indicating that the thread should exit.

    LxssServerPort ServerPort(std::move(ServerPortHandle));
    for (;;)
    {
        try
        {
            // Wait for a client to connect, break out of the loop on disconnect.
            std::unique_ptr<LxssMessagePort> MessagePort;
            if (!NT_SUCCESS(ServerPort.WaitForConnectionNoThrow(&MessagePort)))
            {
                break;
            }

            std::thread([MessagePort = std::move(MessagePort)]() mutable {
                try
                {
                    // Read the create process request from the client.
                    auto CreateProcessMessage = MessagePort->Receive();
                    const auto Message = gsl::make_span(CreateProcessMessage);
                    const auto* Params = gslhelpers::try_get_struct<LX_INIT_CREATE_NT_PROCESS>(Message);
                    THROW_HR_IF(E_INVALIDARG, !Params || (Params->Header.MessageType != LxInitMessageCreateProcess));

                    // Parse the message.
                    CreateProcessParsed Parsed(Message.subspan(offsetof(LX_INIT_CREATE_NT_PROCESS, Common)));

                    // Unmarshal the handles to be used as stdin / stdout / stederr and mark
                    // them as inheritable.
                    static_assert(LX_INIT_STD_FD_COUNT == 3);

                    wil::unique_handle StdHandles[LX_INIT_STD_FD_COUNT];
                    for (ULONG Index = 0; Index < LX_INIT_STD_FD_COUNT; Index += 1)
                    {
                        StdHandles[Index] = MessagePort->UnmarshalVfsFile(Params->StdFdIds[Index]);
                    }

                    // Create the signal pipe to handle resize requests.
                    auto SignalPipe = wsl::windows::common::wslutil::OpenAnonymousPipe(0, true, true);

                    // Launch the process.
                    auto Result = CreateProcess(&Parsed, StdHandles[0].get(), StdHandles[1].get(), StdHandles[2].get());

                    // Send a response back to the init daemon.
                    LX_INIT_CREATE_PROCESS_RESPONSE Response{};
                    Response.Header.MessageType = LxInitMessageCreateProcessResponse;
                    Response.Header.MessageSize = sizeof(Response);
                    Response.Flags = Result.Flags;
                    Response.Result = Result.Status;
                    if (Result.Status == 0)
                    {
                        // Marshal the write end of the signal pipe.
                        const LXBUS_IPC_MESSAGE_MARSHAL_HANDLE_DATA HandleData{HandleToUlong(SignalPipe.second.get()), LxBusIpcMarshalHandleTypeOutput};
                        Response.SignalPipeId = MessagePort->MarshalHandle(&HandleData);
                        SignalPipe.second.reset();

                        // Write the response to the binfmt interpreter.
                        MessagePort->Send(&Response, sizeof(Response));

                        // Process messages from the binfmt interpreter and wait for the
                        // process to exit.
                        LX_INIT_PROCESS_EXIT_STATUS ExitStatus;
                        ExitStatus.Header.MessageType = LxInitMessageExitStatus;
                        ExitStatus.Header.MessageSize = sizeof(ExitStatus);
                        ExitStatus.ExitCode = ProcessInteropMessages(SignalPipe.first.get(), &Result);

                        // Write the exit status to the binfmt interpreter.
                        MessagePort->Send(&ExitStatus, sizeof(ExitStatus));
                    }
                    else
                    {
                        MessagePort->Send(&Response, sizeof(Response));
                    }
                }
                CATCH_LOG()
            }).detach();
        }
        CATCH_LOG()
    }
}

DWORD
wsl::windows::common::interop::VmModeWorkerThread(_In_ wsl::shared::SocketChannel& Channel, _In_ const GUID& VmId, _In_ bool IgnoreExit)
{
    std::vector<gsl::byte> Buffer;

    for (;;)
    {
        auto [Message, Span] = Channel.ReceiveMessageOrClosed<MESSAGE_HEADER>();
        if (Message == nullptr)
        {
            break;
        }

        switch (Message->MessageType)
        {
        case LxInitMessageExitStatus:
        {
            const auto* ExitStatusMessage = gslhelpers::try_get_struct<LX_INIT_PROCESS_EXIT_STATUS>(Span);
            THROW_HR_IF(E_INVALIDARG, !ExitStatusMessage);

            Channel.SendMessage<LX_INIT_PROCESS_EXIT_STATUS>(Span);

            if (!IgnoreExit)
            {
                return ExitStatusMessage->ExitCode;
            }

            break;
        }

        case LxInitMessageCreateProcessUtilityVm:
            THROW_HR_IF(E_INVALIDARG, (Span.size() < sizeof(LX_INIT_CREATE_PROCESS_UTILITY_VM)));

            CreateProcessVmMode(VmId, Span);
            break;

        default:
            THROW_HR_MSG(E_UNEXPECTED, "Unexpected message %d", Message->MessageType);
        }
    }

    return 1;
}
