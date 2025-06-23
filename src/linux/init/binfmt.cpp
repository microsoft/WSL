/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    binfmt.c

Abstract:

    This file contains definitions for the NT interop binfmt interpreter.

--*/

#include <lxbusapi.h>
#include <sys/signalfd.h>
#include <pty.h>
#include <locale.h>
#include <signal.h>
#include "common.h"
#include "binfmt.h"
#include "wslpath.h"
#include <libgen.h>
#include "util.h"
#include "SocketChannel.h"

#define ACCEPT_TIMEOUT (10 * 1000)

#define LOG_STDERR(_str, ...) \
    { \
        fprintf(stderr, _str ": %s\n", ##__VA_ARGS__, (g_Locale ? strerror_l(errno, g_Locale) : strerror(errno))); \
    }

int g_ConsoleFd = -1;
struct termios g_ConsoleInfoBackup;
locale_t g_Locale;

void CreateNtProcessConfigureConsole(PLX_INIT_CREATE_NT_PROCESS_COMMON Common);

std::vector<gsl::byte> CreateNtProcessMessage(LX_MESSAGE_TYPE MessageType, int Argc, char* Argv[]);

int CreateNtProcessUtilityVm(int Argc, char* Argv[]);

int CreateNtProcessWsl(int Argc, char* Argv[]);

bool HasOpenFileDescriptors(struct pollfd* PollDescriptors, int PollDescriptorSize);

void RestoreConsoleState(void);

void WindowSizeChanged(int SignalChannelFd);

int CreateNtProcess(int Argc, char* Argv[])

/*++

Routine Description:

    This routine issues a create NT process request.

Arguments:

    Argc - Supplies the argument count.

    Argv - Supplies the command line arguments.

Return Value:

    The exit code of the launched process on success, 1 on failure.

--*/

{
    //
    // The first argument will be the path of the binfmt interpreter, the second
    // argument will be the full filename of the Windows binary.
    //
    // N.B. The binfmt interpreter is registered with the 'P' flag which preserves
    //      Argv[0] by adding it to the command line after the path of the Windows binary.
    //      https://en.wikipedia.org/wiki/Binfmt_misc
    //

    int ExitCode = 1;
    if (Argc <= 1)
    {
        return ExitCode;
    }

    //
    // Initialize a locale for localized error messages.
    //
    // N.B. Failure to initialize the locale is non-fatal.
    //

    g_Locale = newlocale(LC_ALL_MASK, "", NULL);

    //
    // Check if the binary is being run on WSL or in a Utility VM.
    //

    if (!UtilIsUtilityVm())
    {
        ExitCode = CreateNtProcessWsl(Argc, Argv);
    }
    else
    {
        ExitCode = CreateNtProcessUtilityVm(Argc, Argv);
    }

    RestoreConsoleState();
    return ExitCode;
}

int CreateNtProcessUtilityVm(int Argc, char* Argv[])

/*++

Routine Description:

    This routine issues a create NT process request for VM Mode.

Arguments:

    Argc - Supplies the argument count.

    Argv - Supplies the command line arguments.

Return Value:

    The exit code of the launched process on success, 1 on failure.

--*/
try
{
    int ExitCode = 1;

    //
    // Create the interop message.
    //

    auto Buffer = CreateNtProcessMessage(LxInitMessageCreateProcessUtilityVm, Argc, Argv);
    if (Buffer.empty())
    {
        return ExitCode;
    }

    //
    // Create a listening socket to accept connections for stdin, stdout,
    // stderr, and the control channel.
    //

    sockaddr_vm SocketAddress;
    wil::unique_fd Sockets[LX_INIT_CREATE_NT_PROCESS_SOCKETS];
    wil::unique_fd ListenSocket{UtilListenVsockAnyPort(&SocketAddress, COUNT_OF(Sockets))};
    if (!ListenSocket)
    {
        return ExitCode;
    }

    auto Span = gsl::make_span(Buffer);
    auto* Message = gslhelpers::get_struct<LX_INIT_CREATE_NT_PROCESS_UTILITY_VM>(Span);
    Message->Port = SocketAddress.svm_port;

    //
    // Establish a connection to the interop server.
    //

    wsl::shared::SocketChannel channel{UtilConnectToInteropServer(), "Interop"};
    if (channel.Socket() < 0)
    {
        return ExitCode;
    }

    //
    // Send the create process message to the interop server.
    //

    channel.SendMessage<LX_INIT_CREATE_NT_PROCESS_UTILITY_VM>(Span);

    //
    // Accept connections from the interop server.
    //

    for (int Index = 0; Index < COUNT_OF(Sockets); Index += 1)
    {
        Sockets[Index] = UtilAcceptVsock(ListenSocket.get(), SocketAddress, ACCEPT_TIMEOUT);
        if (!Sockets[Index])
        {
            return ExitCode;
        }
    }

    //
    // Close the listening socket.
    //

    ListenSocket.reset();

    //
    // Create a signalfd to detect window size changes.
    //

    sigset_t SignalMask;
    sigemptyset(&SignalMask);
    sigaddset(&SignalMask, SIGWINCH);
    sigaddset(&SignalMask, SIGINT);
    int Result = sigprocmask(SIG_BLOCK, &SignalMask, NULL);
    if (Result < 0)
    {
        LOG_STDERR("sigprocmask failed %d", errno);
        return ExitCode;
    }

    wil::unique_fd SignalFd{signalfd(-1, &SignalMask, 0)};
    if (!SignalFd)
    {
        LOG_STDERR("signalfd failed %d", errno);
        return ExitCode;
    }

    //
    // Fill output and poll file descriptors.
    //

    int OutputFd[] = {Sockets[0].get(), 1, 2};
    pollfd PollDescriptors[] = {
        {0, POLLIN}, {Sockets[1].get(), POLLIN}, {Sockets[2].get(), POLLIN}, {Sockets[3].get(), POLLIN}, {SignalFd.get(), POLLIN}};

    //
    // Begin relaying from stdin to the stdin socket, and from the stdout and
    // stderr sockets to stdout and stderr.
    //

    while (HasOpenFileDescriptors(PollDescriptors, COUNT_OF(PollDescriptors)))
    {
        Result = poll(PollDescriptors, COUNT_OF(PollDescriptors), -1);
        if (Result <= 0)
        {
            break;
        }

        for (int Index = 0; Index < COUNT_OF(OutputFd); Index += 1)
        {
            if (PollDescriptors[Index].revents & (POLLIN | POLLHUP | POLLERR))
            {
                auto BytesRead = UtilReadBuffer(PollDescriptors[Index].fd, Buffer);
                if (BytesRead == 0)
                {
                    PollDescriptors[Index].fd = -1;
                    if (Index == 0)
                    {
                        if (shutdown(OutputFd[0], SHUT_WR) < 0)
                        {
                            LOG_STDERR("shutdown failed %d", errno);
                        }
                    }
                }
                else if (BytesRead < 0)
                {
                    LOG_STDERR("read failed %d", errno);
                    PollDescriptors[Index].fd = -1;
                }
                else
                {
                    auto BytesWritten = UtilWriteBuffer(OutputFd[Index], Buffer.data(), BytesRead);
                    if (BytesWritten < 0)
                    {
                        LOG_STDERR("write failed %d", errno);
                    }
                }
            }
        }

        //
        // Read the create process response or exit status message from the
        // control channel.
        //

        if (PollDescriptors[3].revents & POLLIN)
        {
            auto PollMessage = wsl::shared::socket::RecvMessage(PollDescriptors[3].fd, Buffer);
            if (PollMessage.empty())
            {
                PollDescriptors[3].fd = -1;
                continue;
            }

            auto* Header = gslhelpers::get_struct<MESSAGE_HEADER>(PollMessage);
            if (Header->MessageType == LxInitMessageCreateProcessResponse)
            {
                //
                // Verify the process launch request was successful.
                //

                auto* Response = gslhelpers::try_get_struct<LX_INIT_CREATE_PROCESS_RESPONSE>(PollMessage);
                if (!Response)
                {
                    LOG_STDERR("Invalid message size %zd", PollMessage.size());
                    break;
                }

                if (Response->Result != 0)
                {
                    errno = Response->Result;
                    LOG_STDERR("%s", Argv[0]);
                    break;
                }

                //
                // If the application was a GUI application and stdin is a console, restore
                // the terminal mode. This allows ctrl-c and ctrl-z to function for
                // graphical apps.
                //

                if ((Response->Flags & LX_INIT_CREATE_PROCESS_RESULT_FLAG_GUI_APPLICATION) != 0)
                {
                    RestoreConsoleState();
                }
            }
            else if (Header->MessageType == LxInitMessageExitStatus)
            {
                //
                // Set the process exit code and go through the relay loop until
                // all data has been flushed.
                //

                auto* ExitStatus = gslhelpers::try_get_struct<LX_INIT_PROCESS_EXIT_STATUS>(PollMessage);
                if (!ExitStatus)
                {
                    LOG_STDERR("Invalid message size %zd", PollMessage.size());
                    break;
                }

                ExitCode = ExitStatus->ExitCode;
                PollDescriptors[3].fd = -1;
            }
            else
            {
                LOG_STDERR("Unexpected message %d", Header->MessageType);
                break;
            }
        }

        //
        // Forward window resize events via the relay pipe and handle sigint.
        //

        if (PollDescriptors[4].revents & POLLIN)
        {
            signalfd_siginfo SignalInfo;
            auto BytesRead = TEMP_FAILURE_RETRY(read(PollDescriptors[4].fd, &SignalInfo, sizeof(SignalInfo)));
            if (BytesRead != sizeof(SignalInfo))
            {
                LOG_STDERR("read failed %zd %d", BytesRead, errno);
                break;
            }

            if (SignalInfo.ssi_signo == SIGWINCH)
            {
                WindowSizeChanged(Sockets[3].get());
            }
            else if (SignalInfo.ssi_signo == SIGINT)
            {
                if (shutdown(OutputFd[0], SHUT_WR) < 0)
                {
                    LOG_STDERR("shutdown failed %d", errno);
                }

                break;
            }
            else
            {
                LOG_STDERR("Unexpected signal %u", SignalInfo.ssi_signo);
                break;
            }
        }

        //
        // Control channel is closed. This means that the windows process has
        // exited. Close stdin channel to unblock the relay thread and disable
        // polling on the stdin and signalfd channels. However, there might be
        // some unread data on the stdout and stderr channels so continue
        // polling/reading on them until EOF is received.
        //

        if (PollDescriptors[3].fd == -1)
        {
            if (shutdown(OutputFd[0], SHUT_WR) < 0)
            {
                LOG_STDERR("shutdown failed %d", errno);
            }

            PollDescriptors[0].fd = -1;
            PollDescriptors[4].fd = -1;
        }
    }

    return ExitCode;
}
CATCH_RETURN_ERRNO()

int CreateNtProcessWsl(int Argc, char* Argv[])

/*++

Routine Description:

    This routine issues a create NT process request for lxcore-based instances.

Arguments:

    Argc - Supplies the argument count.

    Argv - Supplies the command line arguments.

Return Value:

    The exit code of the launched process on success, 1 on failure.

--*/

{
    int ExitCode = 1;

    //
    // Connect to the Windows server that handles create process requests.
    //

    wil::unique_fd LxBusFd{TEMP_FAILURE_RETRY(open(LXBUS_DEVICE_NAME, O_RDWR))};
    if (!LxBusFd)
    {
        return ExitCode;
    }

    LXBUS_CONNECT_SERVER_PARAMETERS ConnectParams{};
    ConnectParams.Input.Flags = LXBUS_IPC_CONNECT_FLAG_UNNAMED_SERVER;
    ConnectParams.Input.TimeoutMs = LXBUS_IPC_INFINITE_TIMEOUT;
    int Result = TEMP_FAILURE_RETRY(ioctl(LxBusFd.get(), LXBUS_IOCTL_CONNECT_SERVER, &ConnectParams));
    if (Result < 0)
    {
        return ExitCode;
    }

    wil::unique_fd CreateProcessFd{ConnectParams.Output.MessagePort};
    std::vector<gsl::byte> Buffer = CreateNtProcessMessage(LxInitMessageCreateProcess, Argc, Argv);
    if (Buffer.empty())
    {
        return ExitCode;
    }

    //
    // Marshal the standard handles.
    //

    auto Span = gsl::make_span(Buffer);
    auto* Message = gslhelpers::get_struct<LX_INIT_CREATE_NT_PROCESS>(Span);
    for (int Index = 0; Index < LX_INIT_STD_FD_COUNT; ++Index)
    {
        LXBUS_IPC_MESSAGE_MARSHAL_VFS_FILE_PARAMETERS MarshalFile{};
        MarshalFile.Input.Fd = Index;
        Result = TEMP_FAILURE_RETRY(ioctl(CreateProcessFd.get(), LXBUS_IPC_MESSAGE_IOCTL_MARSHAL_VFS_FILE, &MarshalFile));
        if (Result < 0)
        {
            return ExitCode;
        }

        Message->StdFdIds[Index] = MarshalFile.Output.VfsFileId;
    }

    //
    // Send the create NT process message to the server.
    //

    auto Bytes = UtilWriteBuffer(CreateProcessFd.get(), Span);
    if (Bytes != static_cast<ssize_t>(Span.size()))
    {
        return ExitCode;
    }

    //
    // Close the file descriptors representing stdin, stdout, and stderr.
    //

    for (int Index = 0; Index < LX_INIT_STD_FD_COUNT; ++Index)
    {
        CLOSE(Index);
    }

    //
    // Create a signalfd to detect window size changes.
    //

    sigset_t SignalMask;
    sigemptyset(&SignalMask);
    sigaddset(&SignalMask, SIGWINCH);
    sigaddset(&SignalMask, SIGINT);
    Result = sigprocmask(SIG_BLOCK, &SignalMask, NULL);
    if (Result < 0)
    {
        LOG_STDERR("sigprocmask failed %d", errno);
        return ExitCode;
    }

    wil::unique_fd SignalFd{signalfd(-1, &SignalMask, 0)};
    if (!SignalFd)
    {
        LOG_STDERR("signalfd failed %d", errno);
        return ExitCode;
    }

    //
    // Initialize poll state.
    //

    pollfd PollDescriptors[2];
    PollDescriptors[0].fd = CreateProcessFd.get();
    PollDescriptors[0].events = POLLIN;
    PollDescriptors[1].fd = SignalFd.get();
    PollDescriptors[1].events = POLLIN;

    //
    // Begin worker loop.
    //

    wil::unique_fd SignalChannelFd{};
    for (;;)
    {
        Result = poll(PollDescriptors, COUNT_OF(PollDescriptors), -1);
        if (Result < 0)
        {
            LOG_STDERR("poll failed %d", errno);
            break;
        }

        //
        // Read the create process response or exit status message from the
        // control channel.
        //

        if (PollDescriptors[0].revents & POLLIN)
        {
            union
            {
                MESSAGE_HEADER Header;
                LX_INIT_PROCESS_EXIT_STATUS ExitStatus;
                LX_INIT_CREATE_PROCESS_RESPONSE Response;
            } Reply;

            Bytes = TEMP_FAILURE_RETRY(read(PollDescriptors[0].fd, &Reply, sizeof(Reply)));
            if (Bytes < 0)
            {
                LOG_STDERR("read failed %d", errno);
                return ExitCode;
            }

            if (Reply.Header.MessageType == LxInitMessageCreateProcessResponse)
            {
                //
                // Verify the process launch request was successful.
                //

                if (Reply.Response.Result != 0)
                {
                    errno = Reply.Response.Result;
                    LOG_STDERR("%s", Argv[0]);
                    return ExitCode;
                }

                //
                // Unmarshal the signal channel if one was created.
                //

                if (Reply.Response.SignalPipeId != 0)
                {
                    LXBUS_IPC_MESSAGE_UNMARSHAL_HANDLE_PARAMETERS UnmarshalHandle{};
                    UnmarshalHandle.Input.HandleId = Reply.Response.SignalPipeId;
                    Result = TEMP_FAILURE_RETRY(ioctl(CreateProcessFd.get(), LXBUS_IPC_MESSAGE_IOCTL_UNMARSHAL_HANDLE, &UnmarshalHandle));
                    if (Result < 0)
                    {
                        return ExitCode;
                    }

                    SignalChannelFd.reset(UnmarshalHandle.Output.FileDescriptor);
                }

                //
                // If the application was a GUI application and stdin is a console, restore
                // the terminal mode. This allows ctrl-c and ctrl-z to function for
                // graphical apps.
                //

                if ((Reply.Response.Flags & LX_INIT_CREATE_PROCESS_RESULT_FLAG_GUI_APPLICATION) != 0)
                {
                    RestoreConsoleState();
                }
            }
            else if (Reply.Header.MessageType == LxInitMessageExitStatus)
            {
                ExitCode = Reply.ExitStatus.ExitCode;
                UtilWriteBuffer(PollDescriptors[0].fd, &Reply, Bytes);
                break;
            }
            else
            {
                LOG_STDERR("Unexpected message");
                break;
            }
        }

        //
        // Forward window resize events via the relay pipe and handle sigint.
        //

        if (PollDescriptors[1].revents & POLLIN)
        {
            signalfd_siginfo SignalInfo;
            Bytes = TEMP_FAILURE_RETRY(read(PollDescriptors[1].fd, &SignalInfo, sizeof(SignalInfo)));
            if (Bytes != sizeof(SignalInfo))
            {
                LOG_STDERR("read failed %zd %d", Bytes, errno);
                break;
            }

            if (SignalInfo.ssi_signo == SIGWINCH)
            {
                WindowSizeChanged(SignalChannelFd.get());
            }
            else if (SignalInfo.ssi_signo == SIGINT)
            {
                break;
            }
            else
            {
                LOG_STDERR("Unexpected signal %u", SignalInfo.ssi_signo);
                break;
            }
        }
    }

    return ExitCode;
}

void CreateNtProcessConfigureConsole(PLX_INIT_CREATE_NT_PROCESS_COMMON Common)

/*++

Routine Description:

    This routine queries stdin, stdout, and stderr and determines if a
    Windows pseudoconsole should be created. It also performs additional logic
    around setting and restoring the terminal mode if stdin is a console.

Arguments:

    Common - Supplies a pointer to the common create process information. This
        buffer will be modified if the console state is inconsistent.

Return Value:

    None.

--*/

{
    struct winsize WindowSize;

    //
    // Ensure that stdin, stdout, and stderr are terminals.
    //

    termios ConsoleInfo;
    for (int Index = 0; Index < LX_INIT_STD_FD_COUNT; Index += 1)
    {
        if (tcgetattr(Index, &ConsoleInfo) < 0)
        {
            return;
        }
    }

    //
    // Ensure that stdin represents the foreground process group.
    // N.B. It's possible that standard file descriptors point to tty while the process
    // has no controlling terminal (in case its parent called setsid() without opening a new terminal for instance).
    // See https://github.com/microsoft/WSL/issues/13173.
    //

    auto processGroup = tcgetpgrp(0);
    if (processGroup < 0)
    {
        if (errno != ENOTTY)
        {
            LOG_STDERR("tcgetpgrp failed");
        }

        return;
    }

    if (processGroup != getpgrp())
    {
        return;
    }

    //
    // Ensure stdin, stdout, and stderr represent the same terminal.
    //

    struct stat StdIn;
    if (fstat(0, &StdIn) < 0)
    {
        LOG_STDERR("fstat(0) failed");
        return;
    }

    struct stat StatBuffer;
    for (int Index = 1; Index < LX_INIT_STD_FD_COUNT; Index += 1)
    {
        if (fstat(Index, &StatBuffer) < 0)
        {
            LOG_STDERR("fstat(%d) failed", Index);
            return;
        }

        if (StatBuffer.st_dev != StdIn.st_dev)
        {
            return;
        }
    }

    //
    // Query the window size.
    //

    if (ioctl(0, TIOCGWINSZ, &WindowSize) < 0)
    {
        LOG_STDERR("ioctl(TIOCGWINSZ) failed");
        return;
    }

    //
    // Don't create a pseudoconsole if either the row or column size is zero.
    //

    if ((WindowSize.ws_row == 0) || (WindowSize.ws_col == 0))
    {
        return;
    }

    Common->Rows = WindowSize.ws_row;
    Common->Columns = WindowSize.ws_col;

    //
    // Set the terminal to raw mode.
    //

    memcpy(&g_ConsoleInfoBackup, &ConsoleInfo, sizeof(ConsoleInfo));
    cfmakeraw(&ConsoleInfo);
    if (TEMP_FAILURE_RETRY(tcsetattr(0, TCSANOW, &ConsoleInfo)) < 0)
    {
        LOG_STDERR("tcsetattr failed");
        return;
    }

    //
    // Duplicate stdin to query window size changes.
    //

    g_ConsoleFd = dup(0);
    if (g_ConsoleFd < 0)
    {
        LOG_STDERR("dup failed");
        return;
    }

    Common->CreatePseudoconsole = true;
}

std::vector<gsl::byte> CreateNtProcessMessage(LX_MESSAGE_TYPE MessageType, int Argc, char* Argv[])

/*++

Routine Description:

    This routine allocates and initializes a create NT process message.

Arguments:

    MessageType - Supplies the message type.

    Argc - Supplies the command line agrument count.

    Argv - Supplies the command line arguments.

Return Value:

    The initialized message buffer.

--*/

try
{
    //
    // Calculate the size of the create process message.
    //

    size_t Size;
    switch (MessageType)
    {
    case LxInitMessageCreateProcess:
        Size = offsetof(LX_INIT_CREATE_NT_PROCESS, Common.Buffer);
        break;

    case LxInitMessageCreateProcessUtilityVm:
        Size = offsetof(LX_INIT_CREATE_NT_PROCESS_UTILITY_VM, Common.Buffer);
        break;

    default:
        return {};
    }

    //
    // Translate the Linux filename into a Windows path.
    //

    auto Filename = WslPathTranslate(Argv[0], (TRANSLATE_FLAG_ABSOLUTE | TRANSLATE_FLAG_RESOLVE_SYMLINKS), TRANSLATE_MODE_WINDOWS);
    if (Filename.empty())
    {
        return {};
    }

    if (UtilSizeTAdd(Filename.length(), Size, &Size) == false)
    {
        return {};
    }

    if (UtilSizeTAdd(1, Size, &Size) == false)
    {
        return {};
    }

    //
    // Attempt to translate the current working directory, if translation fails
    // use an empty current working directory.
    //

    auto Cwd = std::filesystem::current_path().string();
    auto CurrentWorkingDirectory = WslPathTranslate(Cwd.data(), TRANSLATE_FLAG_ABSOLUTE, TRANSLATE_MODE_WINDOWS);
    if (UtilSizeTAdd(CurrentWorkingDirectory.length(), Size, &Size) == false)
    {
        return {};
    }

    if (UtilSizeTAdd(1, Size, &Size) == false)
    {
        return {};
    }

    //
    // Initialize the environment.
    //

    auto Environment = UtilParseWslEnv(nullptr);
    if (UtilSizeTAdd(Environment.size(), Size, &Size) == false)
    {
        return {};
    }

    if (UtilSizeTAdd(1, Size, &Size) == false)
    {
        return {};
    }

    //
    // If Argv[0] and Argv[1] match, use the basename for Argv[1].
    // This is useful for Windows binaries that inspect the first argument.
    //
    // N.B. Arg[1] cannot be passed as-is because some windows binaries (like cmd.exe)
    //      do not handle the Linux-style path.
    //

    if ((Argc > 1) && (strcmp(Argv[0], Argv[1])) == 0)
    {
        Argv[1] = basename(Argv[1]);
    }

    //
    // Calculate the size of the command line.
    //

    for (int Index = 1; Index < Argc; Index += 1)
    {
        if (UtilSizeTAdd(strlen(Argv[Index]), Size, &Size) == false)
        {
            return {};
        }

        if (UtilSizeTAdd(1, Size, &Size) == false)
        {
            return {};
        }
    }

    if (Size > ULONG_MAX)
    {
        return {};
    }

    //
    // Initialize the message.
    //

    std::vector<gsl::byte> Buffer(Size);
    auto Message = gsl::make_span(Buffer);
    auto* Header = gslhelpers::get_struct<MESSAGE_HEADER>(Message);
    Header->MessageType = MessageType;
    Header->MessageSize = static_cast<unsigned>(Size);
    Message = Message.subspan(
        (MessageType == LxInitMessageCreateProcess) ? offsetof(LX_INIT_CREATE_NT_PROCESS, Common)
                                                    : offsetof(LX_INIT_CREATE_NT_PROCESS_UTILITY_VM, Common));

    auto* Common = gslhelpers::get_struct<LX_INIT_CREATE_NT_PROCESS_COMMON>(Message);
    size_t Offset = offsetof(LX_INIT_CREATE_NT_PROCESS_COMMON, Buffer);

    //
    // Copy filename, cwd, environment into the message buffer.
    //

    Common->FilenameOffset = wsl::shared::string::CopyToSpan(Filename, Message, Offset);
    Common->CurrentWorkingDirectoryOffset = wsl::shared::string::CopyToSpan(CurrentWorkingDirectory, Message, Offset);
    Common->EnvironmentOffset = wsl::shared::string::CopyToSpan(std::string_view{Environment.data(), Environment.size()}, Message, Offset);

    //
    // Copy the command line arguments.
    //

    Common->CommandLineOffset = gsl::narrow_cast<unsigned int>(Offset);
    Common->CommandLineCount = gsl::narrow_cast<unsigned short>(Argc - 1);
    for (int Index = 1; Index < Argc; Index += 1)
    {
        wsl::shared::string::CopyToSpan(Argv[Index], Message, Offset);
    }

    //
    // Initialize the console state.
    //

    CreateNtProcessConfigureConsole(Common);

    //
    // Return the message to the caller.
    //

    return Buffer;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

bool HasOpenFileDescriptors(struct pollfd* PollDescriptors, int PollDescriptorSize)
/*++

Routine Description:

    This routine checks if the given array of PollDescriptors has any
    PollDescriptor that is still open. In other words, it checks if there
    is at least one descriptor that has a fd >= 0.

Arguments:

    PollDescriptors - The array of poll descriptors.
    PollDescriptorSize - The count of number of elements in the
                    PollDescriptors array.

Return Value:

    True if there is at least one open poll descriptor, False otherwise.

--*/
{
    for (int i = 0; i < PollDescriptorSize; i++)
    {
        if (PollDescriptors[i].fd >= 0)
        {
            return true;
        }
    }

    return false;
}

void RestoreConsoleState(void)

/*++

Routine Description:

    This routine restores the original console state.

Arguments:

    None.

Return Value:

    None.

--*/

{
    if (g_ConsoleFd != -1)
    {
        tcsetattr(g_ConsoleFd, TCSANOW, &g_ConsoleInfoBackup);
        CLOSE(g_ConsoleFd);
        g_ConsoleFd = -1;
    }
}

void WindowSizeChanged(int SignalChannelFd)

/*++

Routine Description:

    This routine is the signal handler interop window size changes.

Arguments:

    SignalChannelFd - Supplies a file descriptor to write the window size
        message.

Return Value:

    None.

--*/

{
    if ((SignalChannelFd == -1) || (g_ConsoleFd == -1))
    {
        return;
    }

    //
    // Query the new window size and send the updated size via the signal
    // channel.
    //

    winsize WindowSize;
    int Result = ioctl(g_ConsoleFd, TIOCGWINSZ, &WindowSize);
    if (Result < 0)
    {
        LOG_STDERR("ioctl(TIOCGWINSZ) failed");
        return;
    }

    LX_INIT_WINDOW_SIZE_CHANGED ResizeMessage;
    ResizeMessage.Header.MessageType = LxInitMessageWindowSizeChanged;
    ResizeMessage.Header.MessageSize = sizeof(ResizeMessage);
    ResizeMessage.Columns = WindowSize.ws_col;
    ResizeMessage.Rows = WindowSize.ws_row;
    Result = UtilWriteBuffer(SignalChannelFd, gslhelpers::struct_as_bytes(ResizeMessage));
    if (Result < 0)
    {
        LOG_STDERR("sending resize message failed");
    }
}
