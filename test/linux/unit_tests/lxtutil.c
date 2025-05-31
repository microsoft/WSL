/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    lxtlog.c

Abstract:

    This file contains lx test logging routines.

--*/

#include "lxtutil.h"
#include "lxtlog.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <linux/futex.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>

#define SIGNAL_WAIT_COUNT (20)
#define SIGNAL_WAIT_TIMEOUT_US (100000)
#define SIGNAL_MAX_SIGNALS (10)
#define SIGNAL_MAX_THREADS (5)

typedef struct _LXT_SIGNAL_INFO
{
    pid_t ThreadId;
    int ReceivedSignal[SIGNAL_MAX_SIGNALS];
    siginfo_t SignalInfo[SIGNAL_MAX_SIGNALS];
    BOOLEAN AllowMultipleSignals;
    int SignalCount;
} LXT_SIGNAL_INFO, *PLXT_SIGNAL_INFO;

typedef struct _LXT_TYPE_MAPPING
{
    char Type;
    mode_t Mode;
} LXT_TYPE_MAPPING, *PLXT_TYPE_MAPPING;

void LxtPrintPartialMemory(const unsigned char* Buffer, size_t Size, size_t BufferIndex, const char* Prefix);

void LxtShowUsage(PCLXT_VARIATION Variations, unsigned int VariationCount);

PLXT_SIGNAL_INFO
LxtSignalFindThreadInfo(void);

void LxtSignalHandler(int Signal);

void LxtSignalHandlerSigAction(int Signal, siginfo_t* SigInfo, void* UContext);

//
// The multi-threaded signal tests require that information about the last
// signal received is stored per-thread, however using thread local storage
// is not safe in a signal handler (TLS support may take locks, if a signal
// arrives while the lock is held and the signal handler then tries to take
// the same lock, it leads to deadlock). Instead, an array is used that
// stores information for each thread.
//

static LXT_SIGNAL_INFO g_ThreadSignalInfo[SIGNAL_MAX_THREADS];
static int g_NextSignalThread = 0;
static LXT_TYPE_MAPPING g_TypeMapping[] = {
    {DT_REG, S_IFREG}, {DT_DIR, S_IFDIR}, {DT_LNK, S_IFLNK}, {DT_FIFO, S_IFIFO}, {DT_SOCK, S_IFSOCK}, {DT_CHR, S_IFCHR}, {DT_BLK, S_IFBLK}};

static int g_WslVersion = 0;

//
// Test framework code
//

int LxtCheckDirectoryContents(const char* Path, const LXT_CHILD_INFO* Children, size_t Count)

/*++

Description:

    This routine tests if the specified children are present in the directory.

Arguments:

    Path - Supplies the path of the directory.

    Children - Supplies the list of expected children.

    Count - Supplies the number of children.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    return LxtCheckDirectoryContentsEx(Path, Children, Count, LXT_CHECK_DIRECTORY_CONTENTS_READ_FILES);
}

int LxtCheckDirectoryContentsEx(const char* Path, const LXT_CHILD_INFO* Children, size_t Count, int Flags)

/*++

Description:

    This routine tests if the specified children are present in the directory.

Arguments:

    Path - Supplies the path of the directory.

    Children - Supplies the list of expected children.

    Count - Supplies the number of children.

    Flags - Supplies the flags.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    DIR* Directory;
    struct dirent* Entry;
    BOOLEAN* FoundEntries;
    char FullPath[1024];
    size_t Index;
    int Result;
    struct stat Stat;

    FoundEntries = malloc(Count * sizeof(BOOLEAN));
    memset(FoundEntries, 0, Count * sizeof(BOOLEAN));
    Directory = opendir(Path);
    if (Directory == NULL)
    {
        LxtLogError("opendir failed, errno: %d (%s)", errno, strerror(errno));
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    errno = 0;
    while ((Entry = readdir(Directory)) != NULL)
    {
        LxtLogInfo(
            "Entry %p - d_name: %s d_ino: %llu d_type: %d d_off: %d d_reclen: %d",
            Entry,
            Entry->d_name,
            Entry->d_ino,
            Entry->d_type,
            Entry->d_off,
            Entry->d_reclen);

        for (Index = 0; Index < Count; Index += 1)
        {
            if (strcmp(Children[Index].Name, Entry->d_name) == 0)
            {
                if (FoundEntries[Index] != FALSE)
                {
                    LxtLogError("Duplicate entry '%s'", Entry->d_name);
                    Result = LXT_RESULT_FAILURE;
                    goto ErrorExit;
                }

                LxtCheckEqual(FoundEntries[Index], FALSE, "%d");
                LxtCheckGreater(Entry->d_ino, 0, "%llu");
                LxtCheckEqual(Entry->d_type, Children[Index].FileType, "%d");
                FoundEntries[Index] = TRUE;
                strcpy(FullPath, Path);
                strcat(FullPath, "/");
                strcat(FullPath, Entry->d_name);
                LxtCheckResult(LxtCheckStat(FullPath, Entry->d_ino, Children[Index].FileType));

                if ((Flags & LXT_CHECK_DIRECTORY_CONTENTS_READ_FILES) != 0)
                {
                    LxtCheckResult(LxtCheckRead(FullPath, Children[Index].FileType));
                }
            }
        }
    }

    if (errno != 0)
    {
        LxtLogError("readdir failed; errno: %d (%s)", errno, strerror(errno));
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    //
    // Check if all the required entries have been found.
    //

    for (Index = 0; Index < Count; Index += 1)
    {
        if (FoundEntries[Index] == FALSE)
        {
            LxtLogError("Entry '%s' is missing", Children[Index].Name);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Directory != NULL)
    {
        closedir(Directory);
    }

    if (FoundEntries != NULL)
    {
        free(FoundEntries);
    }

    return Result;
}

int LxtCheckFdPath(int Fd, char* ExpectedPath)

/*++

Description:

    This routine checks if the file descriptor has the specified path.

Arguments:

    Fd - Supplies the file descriptor.

    ExpectedPath - Supplies the expected path.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char ProcFsPath[PATH_MAX];
    char Path[PATH_MAX];
    int Result;

    sprintf(ProcFsPath, "/proc/self/fd/%d", Fd);
    LxtCheckResult(LxtCheckLinkTarget(ProcFsPath, ExpectedPath));

ErrorExit:
    return Result;
}

int LxtCheckLinkTarget(const char* Path, const char* ExpectedTarget)

/*++

Description:

    This routine tests the target of the specified link.

Arguments:

    Path - Supplies the path of the link.

    ExpectedTarget - Supplies the expected target of the link.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[256] = {0};
    int Result;
    ssize_t Size;

    LxtCheckErrno(Size = readlink(Path, Buffer, sizeof(Buffer)));
    LxtCheckEqual((size_t)Size, strlen(Buffer), "%d");
    LxtCheckStringEqual(ExpectedTarget, Buffer);

ErrorExit:
    return Result;
}

int LxtCheckRead(const char* FullPath, unsigned char FileType)

/*++

Description:

    This routine checks that the specified file can be read.

    N.B. This only checks that the file can be opened and read, it doesn't
         check if the contents match what's expected. Write additional tests
         for a specific file if necessary.

Arguments:

    FullPath - Supplies the full path of the file or directory.

    FileType - Supplies the file type.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[1024];
    int Fd;
    int Result;
    ssize_t Size;
    struct stat Stat;

    Fd = 0;
    switch (FileType)
    {
    case DT_REG:

        //
        // Skip files that aren't readable.
        //

        LxtCheckErrnoZeroSuccess(lstat(FullPath, &Stat));
        if ((Stat.st_mode & S_IRUSR) == 0)
        {
            Result = LXT_RESULT_SUCCESS;
            goto ErrorExit;
        }

        LxtCheckErrno(Fd = open(FullPath, O_RDONLY));
        LxtCheckErrno(Size = read(Fd, Buffer, sizeof(Buffer)));
        LxtCheckGreater(Size, 0, "%d");
        break;

    case DT_LNK:
        LxtCheckErrno(Size = readlink(FullPath, Buffer, sizeof(Buffer)));
        LxtCheckGreater(Size, 0, "%d");
        break;

    case DT_DIR:

        //
        // Nothing to check.
        //

        Result = LXT_RESULT_SUCCESS;
        break;

    default:
        LxtLogError("Unexpected file type %d", FileType);
        Result = LXT_RESULT_FAILURE;
        break;
    }

ErrorExit:
    if (Result < 0)
    {
        LxtLogError("Error reading %s", FullPath);
    }

    if (Fd > 0)
    {
        close(Fd);
    }

    return Result;
}

int LxtCheckStat(const char* FullPath, unsigned long long ExpectedInode, unsigned char FileType)

/*++

Description:

    This routine checks the stat information for a file or directory.

Arguments:

    FullPath - Supplies the full path of the file or directory.

    ExpectedInode - Supplies the expected inode number.

    FileType - Supplies the file type.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Index;
    int Result;
    struct stat Stat;

    LxtCheckErrnoZeroSuccess(lstat(FullPath, &Stat));
    LxtCheckEqual(Stat.st_ino, ExpectedInode, "%llu");
    LxtCheckGreater(Stat.st_nlink, 0, "%ud");
    for (Index = 0; Index < LXT_COUNT_OF(g_TypeMapping); Index += 1)
    {
        if (g_TypeMapping[Index].Type == FileType)
        {
            LxtCheckEqual((Stat.st_mode & S_IFMT), g_TypeMapping[Index].Mode, "0%o");

            break;
        }
    }

    if (Index == LXT_COUNT_OF(g_TypeMapping))
    {
        LxtLogError("Unexpected file type %d", FileType);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

ErrorExit:
    return Result;
}

int LxtCheckWrite(const char* FullPath, const char* Value)

/*++

Description:

    This routine checks that the specified file can be written to.

    N.B. This function is meant for writable files in /proc and /sys. It's
         primarily used for files that currently don't have a real write
         implementation (which allow but silently ignore the write) since the
         effects of the write are not checked.

Arguments:

    FullPath - Supplies the full path of the file or directory.

    FileType - Supplies the file type.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesWritten;
    int Fd;
    int Result;

    LxtCheckErrno(Fd = open(FullPath, O_WRONLY));
    LxtCheckErrno(BytesWritten = write(Fd, Value, strlen(Value)));
    LxtCheckEqual((size_t)BytesWritten, strlen(Value), "%d");

ErrorExit:
    if (Result < 0)
    {
        LxtLogError("Error writing %s", FullPath);
    }

    if (Fd > 0)
    {
        close(Fd);
    }

    return Result;
}

int LxtCheckWslPathTranslation(char* Path, const char* ExpectedPath, bool WinPath)

/*++

Description:

    This routine checks whether translating a path with wslpath matches the
    specified result.

Arguments:

    Path - Supplies the path to translate.

    ExpectedPath - Supplies the expected translated path.

    WinPath - Supplies a value that indicates whether the specified path is a
        Windows path. When true, the expected path must be a Linux path and
        vice versa.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    char TranslatedPath[4096];

    LxtCheckResult(LxtExecuteWslPath(Path, WinPath, TranslatedPath, sizeof(TranslatedPath)));
    LxtCheckStringEqual(ExpectedPath, TranslatedPath);
    LxtLogInfo("%s => %s", Path, TranslatedPath);

ErrorExit:
    return Result;
}

int LxtExecuteAndReadOutput(char** Argv, char* OutputBuffer, size_t OutputBufferSize)

/*++

Description:

    This routine runs an executable, and reads stdout into the specified buffer.

    N.B. If the process produces more output than fits in the buffer, this
         function will fail.

Arguments:

    Argv - Supplies the arguments to pass to the executable. The first element
        is the executable to run.

    OutputBuffer - Supplies the buffer to hold the process's stdout.

    OutputBufferSize - Supplies the size of the output buffer.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    ssize_t BytesRead;
    pid_t ChildPid;
    int Result;
    LXT_PIPE Pipe = {-1, -1};

    LxtCheckResult(LxtCreatePipe(&Pipe));
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckClose(Pipe.Read);
        LxtCheckErrno(dup2(Pipe.Write, STDOUT_FILENO));
        LxtCheckClose(Pipe.Write);
        LxtCheckErrno(execve(Argv[0], Argv, environ));
        _exit(LXT_RESULT_FAILURE);
    }

    LxtCheckClose(Pipe.Write);
    while ((BytesRead = read(Pipe.Read, OutputBuffer, OutputBufferSize)) > 0)
    {
        OutputBuffer += BytesRead;
        OutputBufferSize -= BytesRead;
        LxtCheckGreater(OutputBufferSize, 0, "%lu");
    }

    //
    // Make sure the result did not exceed the buffer size and NULL-terminate
    // it.
    //

    LxtCheckErrnoZeroSuccess(BytesRead);
    LxtCheckGreater(OutputBufferSize, BytesRead, "%lu");
    OutputBuffer[BytesRead] = '\0';

    //
    // Make sure the executable exited successfully.
    //

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

ErrorExit:
    LxtClosePipe(&Pipe);

    return Result;
}

int LxtExecuteWslPath(char* Path, bool WinPath, char* OutputBuffer, size_t OutputBufferSize)

/*++

Description:

    This routine runs wslpath, and reads stdout into the specified buffer.

    N.B. If the process produces more output than fits in the buffer, this
         function will fail.

Arguments:

    Path - Supplies the path to translate.

    WinPath - Supplies a value that indicates whether the specified path is a
        Windows path. When true, the output will be a Linux path and vice versa.

    OutputBuffer - Supplies the buffer to hold the process's stdout.

    OutputBufferSize - Supplies the size of the output buffer.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char* Argv[4];
    int Index = 0;
    int Result;
    size_t OutputLength;

    //
    // Construct the arguments to invoke wslpath.
    //

    Argv[Index++] = "/bin/wslpath";
    if (WinPath == false)
    {
        Argv[Index++] = "-w";
    }

    Argv[Index++] = Path;
    Argv[Index] = NULL;

    //
    // Execute wslpath.
    //

    LxtCheckResult(LxtExecuteAndReadOutput(Argv, OutputBuffer, OutputBufferSize));

    //
    // Wslpath outputs a new line at the end. Strip it to make things easier on
    // the caller.
    //

    OutputLength = strlen(OutputBuffer);
    if ((OutputLength > 0) && (OutputBuffer[OutputLength - 1] == '\n'))
    {
        OutputBuffer[OutputLength - 1] = '\0';
    }

ErrorExit:
    return Result;
}

int LxtInitialize(int Argc, char* Argv[], PLXT_ARGS Args, const char* TestName)

/*++
--*/

{

    int Opt;
    int OriginalOptErr;
    int Result;

    //
    // Set umask to 0 so files created by tests have the expected permissions.
    //

    Result = umask(0);
    if (Result < 0)
    {
        LxtLogError("umask failed %d", errno);
        goto ErrorExit;
    }

    //
    // Parse the command line, ignore unrecognized options since variations can
    // specify their own options, and initialize logging.
    //

    Args->LogType = LXT_LOG_TYPE_DEFAULT_MASK;
    Args->LogAppend = false;
    Args->HelpRequested = false;
    Args->VariationMask = -1;
    Args->Argc = Argc;
    Args->Argv = Argv;
    OriginalOptErr = opterr;
    opterr = 0;
    while ((Opt = getopt(Argc, Argv, "l:v:a:h")) != LXT_RESULT_FAILURE)
    {
        switch (Opt)
        {
        case 'a':
            Args->LogAppend = true;
            break;

        case 'l':
            Args->LogType = atoi(optarg);
            if (Args->LogType >= LxtLogTypeMax)
            {
                Result = LXT_RESULT_FAILURE;
                LxtLogError("Invalid LxtLogType %d", Args->LogType);
                goto ErrorExit;
            }

            break;

        case 'v':
            Args->VariationMask = atoll(optarg);
            break;

        case 'h':
            Args->HelpRequested = true;
            break;
        }
    }

    opterr = OriginalOptErr;
    Result = LxtLogInitialize(TestName, Args->LogType, Args->LogAppend);

ErrorExit:
    return Result;
}

int LxtRunVariations(PLXT_ARGS Args, PCLXT_VARIATION Variations, unsigned int VariationCount)

/*++
--*/

{

    unsigned int Itr;
    unsigned long long ThisVariation;
    int Result;

    Result = LXT_RESULT_FAILURE;
    if (Args->HelpRequested != false)
    {
        LxtShowUsage(Variations, VariationCount);
        LxtLogError("No tests executed.");
        goto ErrorExit;
    }

    for (Itr = 0; Itr < VariationCount; Itr++)
    {
        ThisVariation = (1ull << Itr);

        //
        // TODO: Currently, variation mask is only supported for the first 64
        //       variations.
        //

        if ((Args->VariationMask != 0) && ((ThisVariation & Args->VariationMask) == 0))
        {

            continue;
        }

        LxtLogStart("%s", Variations[Itr].Name);
        Result = Variations[Itr].Variation(Args);
        if (LXT_SUCCESS(Result) == 0)
        {
            LxtLogError("%s", Variations[Itr].Name);
            goto ErrorExit;
        }

        LxtLogPassed("%s", Variations[Itr].Name);
    }

ErrorExit:
    return Result;
}

int LxtRunVariationsForked(PLXT_ARGS Args, PCLXT_VARIATION Variations, unsigned int VariationCount)

/*++

Routine Description:

    This routine runs test variations, with each variation executing in its
    own child process. Use this function if a test may change process state
    that interferes with other tests.

Arguments:

    Args - Supplies the command line arguments.

    Variations - Supplies a pointer to an array of variations.

    VariationCount - Supplies the number items in the variations array.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int ChildPid;
    unsigned int Itr;
    unsigned long long ThisVariation;
    int Result;

    ChildPid = -1;
    Result = LXT_RESULT_FAILURE;
    if (Args->HelpRequested != false)
    {
        LxtShowUsage(Variations, VariationCount);
        LxtLogError("No tests executed.");
        goto ErrorExit;
    }

    for (Itr = 0; Itr < VariationCount; Itr++)
    {
        ThisVariation = (1ull << Itr);

        //
        // TODO: Currently, variation mask is only supported for the first 64
        //       variations.
        //

        if ((Args->VariationMask != 0) && ((ThisVariation & Args->VariationMask) == 0))
        {

            continue;
        }

        LxtCheckResult(ChildPid = fork());
        if (ChildPid == 0)
        {
            LxtLogStart("%s", Variations[Itr].Name);
            Result = Variations[Itr].Variation(Args);
            if (LXT_SUCCESS(Result) == 0)
            {
                LxtLogError("%s", Variations[Itr].Name);
                goto ErrorExit;
            }

            LxtLogPassed("%s", Variations[Itr].Name);
            _exit(0);
        }

        Result = LxtWaitPidPollOptions(ChildPid, 0, 0, 120);
        if (Result < 0)
        {
            LxtLogError("Test execution timed out.");
            kill(ChildPid, SIGKILL);
            Result = LXT_RESULT_FAILURE;
            goto ErrorExit;
        }
    }

ErrorExit:
    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

void LxtUninitialize(void)

/*++
--*/

{

    LxtLogUninitialize();
    return;
}

//
// stdlib wrappers
//

void* LxtAlloc(size_t Size)

/*++
--*/

{

    void* Allocation;

    Allocation = malloc(Size);
    if (Allocation == NULL)
    {
        LxtLogResourceError("malloc failed for size %d", Size);
    }

    return Allocation;
}

void LxtFree(void* Allocation)

/*++
--*/

{

    free(Allocation);
    return;
}

//
// syscall wrappers
//

#define LXT_WAITPID_WAIT_TIMEOUT_US 100000
#define LXT_MESSAGE_WAIT_TIMEOUT_US 100000
#define LXT_MESSAGE_WAIT_COUNT 20

int LxtClone(int (*Entry)(void* Parameter), void* Parameter, int Flags, PLXT_CLONE_ARGS Args)

/*++
--*/

{

    char* ChildStack;
    int Result;

    Args->Stack = LxtAlloc(LXT_CLONE_STACK_SIZE);
    if (Args->Stack == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    memset(Args->Stack, 0, LXT_CLONE_STACK_SIZE);
    ChildStack = Args->Stack + LXT_CLONE_STACK_SIZE;
    LxtCheckErrno(Args->CloneId = clone(Entry, ChildStack, Flags, Parameter, 0, 0, 0));

ErrorExit:
    if (LXT_SUCCESS(Result) == 0)
    {
        LxtFree(Args->Stack);
        Args->Stack = NULL;
    }

    return Result;
}

int LxtClosePipe(PLXT_PIPE Pipe)

/*++
--*/

{

    int Result;

    if (Pipe->Read != -1)
    {
        LxtCheckErrno(close(Pipe->Read));
        Pipe->Read = -1;
    }

    if (Pipe->Write != -1)
    {
        LxtCheckErrno(close(Pipe->Write));
        Pipe->Write = -1;
    }

    Result = 0;

ErrorExit:
    return Result;
}

int LxtCompareMemory(const void* First, const void* Second, size_t Size, const char* FirstDescription, const char* SecondDescription)

/*++

Routine Description:

    This routine compares two memory locations, and if they are different logs
    information about where they are different.

Arguments:

    First - Supplies the address of the first memory location.

    Second - Supplies the address of the second memory location.

    Size - Supplies the size of the memory locations.

    FirstDescription - Supplies the description of the first memory location.

    SecondDescription - Supplies the description of the second memory location.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    size_t End;
    const unsigned char* FirstBytes;
    size_t DifferentIndex;
    size_t Index;
    int Result;
    const unsigned char* SecondBytes;

    Result = LXT_RESULT_SUCCESS;
    FirstBytes = First;
    SecondBytes = Second;
    for (Index = 0; Index < Size; Index += 1)
    {
        if (FirstBytes[Index] != SecondBytes[Index])
        {
            Result = LXT_RESULT_FAILURE;
            break;
        }
    }

    if (Result != LXT_RESULT_SUCCESS)
    {
        LxtLogError(
            "Memory contents of '%s' [1] differ from '%s' [2] at "
            "offset %ld",
            FirstDescription,
            SecondDescription,
            Index);

        LxtPrintPartialMemory(FirstBytes, Size, Index, "[1]:");
        LxtPrintPartialMemory(SecondBytes, Size, Index, "[2]:");
    }

    return Result;
}

int LxtCopyFile(const char* Source, const char* Destination)

/*++

Description:

    This routine copies a file.

Arguments:

    Source - Supplies the source.

    Destination - Supplies the destination.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[4096];
    ssize_t BytesRead;
    int FdDest;
    int FdSource;
    int Result;
    struct stat Stat;

    FdDest = -1;
    FdSource = -1;
    LxtCheckErrno(FdSource = open(Source, O_RDONLY));
    LxtCheckErrnoZeroSuccess(fstat(FdSource, &Stat));
    LxtCheckErrno(FdDest = creat(Destination, Stat.st_mode & ~S_IFMT));
    do
    {
        LxtCheckErrno(BytesRead = read(FdSource, Buffer, sizeof(Buffer)));
        if (BytesRead > 0)
        {
            LxtCheckErrno(write(FdDest, Buffer, BytesRead));
        }
    } while (BytesRead > 0);

ErrorExit:
    if (FdDest >= 0)
    {
        close(FdDest);
    }

    if (FdSource >= 0)
    {
        close(FdSource);
    }

    return Result;
}

int LxtCreatePipe(PLXT_PIPE Pipe)

/*++
--*/

{

    int Result;

    memset(Pipe, -1, sizeof(*Pipe));
    LxtCheckErrno(pipe((int*)Pipe));

ErrorExit:
    return Result;
}

int LxtJoinThread(pid_t* Tid)

{

    pid_t CurrentTid;

    while ((CurrentTid = *(volatile pid_t*)Tid) != 0)
    {
        if (syscall(SYS_futex, Tid, FUTEX_WAIT, CurrentTid, NULL, NULL, 0) < 0 && errno != EAGAIN)
        {
            return -1;
        }
    }

    return 0;
}

int LxtReceiveMessage(int Socket, const char* ExpectedMessage)

/*++

Routine Description:

    This routine receives a message from a socket and checks if it was the
    expected message

Arguments:

    Socket - Supplies a file descriptor for the socket to send on.

    ExpectedMessage - Supplies a pointer to a zero-terminated string containing
        the expected message.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int ExpectedMessageSize;
    char Message[100];
    int MessageSize;
    int Result;
    int WaitCount;

    ExpectedMessageSize = strlen(ExpectedMessage);
    memset(Message, 0, sizeof(Message));
    for (WaitCount = 0; WaitCount < LXT_MESSAGE_WAIT_COUNT; WaitCount += 1)
    {
        MessageSize = recv(Socket, Message, sizeof(Message), MSG_DONTWAIT);
        if (MessageSize >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        {
            break;
        }

        usleep(LXT_MESSAGE_WAIT_TIMEOUT_US);
    }

    if (WaitCount == LXT_MESSAGE_WAIT_COUNT)
    {
        LxtLogError("Receiving the message timed out.");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    LxtCheckErrno(MessageSize);
    if (MessageSize != ExpectedMessageSize)
    {
        LxtLogError("Received %i bytes, expected %i", MessageSize, ExpectedMessageSize);

        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    if (strncmp(Message, ExpectedMessage, ExpectedMessageSize) != 0)
    {
        LxtLogError("Received '%s', expected '%s'", Message, ExpectedMessage);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

void LxtPrintPartialMemory(const unsigned char* Buffer, size_t Size, size_t BufferIndex, const char* Prefix)

/*++

Routine Description:

    This routine prints the contents of a memory buffer at the specified index
    with some context.

Arguments:

    Buffer - Supplies the memory buffer to print.

    Size - Supplies the size of the buffer.

    BufferIndex - Supplies the index at which to print.

    Prefix - Supplies the prefix for the message.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    size_t End;
    size_t Index;
    char Message[256];
    char Temp[10];

    memset(Message, 0, sizeof(Message));
    Index = BufferIndex - 5;
    if (Index > BufferIndex)
    {
        Index = 0;
    }

    End = Index + 11;
    if (End > Size)
    {
        End = Size;
    }

    if (Prefix != NULL)
    {
        strcat(Message, Prefix);
        strcat(Message, " ");
    }

    if (Index > 0)
    {
        strcat(Message, "...");
    }

    for (; Index < End; Index += 1)
    {
        strcat(Message, " ");
        if (Index == BufferIndex)
        {
            strcat(Message, "(");
        }

        sprintf(Temp, "%02x", Buffer[Index]);
        strcat(Message, Temp);
        if (Index == BufferIndex)
        {
            strcat(Message, ")");
        }
    }

    if (End < Size)
    {
        strcat(Message, " ...");
    }

    LxtLogInfo("%s", Message);
    return;
}

int LxtSendMessage(int Socket, const char* Message)

/*++

Routine Description:

    This routine sends a message to a socket and checks if it was successfully
    sent.

Arguments:

    Socket - Supplies a file descriptor for the socket to send on.

    Message - Supplies a pointer to a zero-terminated buffer containing the
        message.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int MessageSize;
    int Result;
    int SentSize;

    MessageSize = strlen(Message);
    LxtCheckErrno(SentSize = send(Socket, Message, MessageSize, 0));
    if (SentSize != MessageSize)
    {
        LxtLogError("Sent %i bytes, expected %i", SentSize, MessageSize);
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

void LxtShowUsage(PCLXT_VARIATION Variations, unsigned int VariationCount)

/*++

Description:

    This routine shows usage for the variations.

Arguments:

    Variations - Supplies the variations.

    VariationCount - Supplies the number of variations.

Return Value:

    None.

--*/

{

    size_t Index;

    LxtLogInfo("Usage: ./test_name [-v <variation_mask>] [-l <log_type>] [-a] [-?]");
    LxtLogInfo("Variations:");
    for (Index = 0; Index < VariationCount; Index += 1)
    {
        LxtLogInfo("%s: %llu", Variations[Index].Name, 1ull << Index);
    }

    return;
}

int LxtSignalBlock(int Signal)

/*++

Routine Description:

    This routine blocks the specified signal.

Arguments:

    Signal - Supplies the signal number.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    sigset_t Signals;

    sigemptyset(&Signals);
    sigaddset(&Signals, Signal);
    LxtCheckErrnoZeroSuccess(sigprocmask(SIG_BLOCK, &Signals, NULL));

ErrorExit:
    return Result;
}

int LxtSignalDefault(int Signal)

/*++

Routine Description:

    This routine reverts to the default action for the specified signal.

Arguments:

    Signal - Supplies the signal number.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct sigaction Action;
    int Result;

    memset(&Action, 0, sizeof(Action));
    Action.sa_handler = SIG_DFL;
    LxtCheckErrnoZeroSuccess(sigaction(Signal, &Action, NULL));

ErrorExit:
    return Result;
}

int LxtSignalIgnore(int Signal)

/*++

Routine Description:

    This routine ignores the specified signal.

Arguments:

    Signal - Supplies the signal number.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct sigaction Action;
    int Result;

    memset(&Action, 0, sizeof(Action));
    Action.sa_handler = SIG_IGN;
    LxtCheckErrnoZeroSuccess(sigaction(Signal, &Action, NULL));

ErrorExit:
    return Result;
}

int LxtSignalCheckInfoReceived(int Signal, int Code, pid_t Pid, uid_t Uid)

/*++

Routine Description:

    This routine checks if the specified signal was received by the signal
    handlers, with the specified info values.

    N.B. The signal handler must have been established with SA_SIGINFO for this
         to work.

Arguments:

    Signal - Supplies the expected signal number.

    Code - Supplies the expected signal code.

    Pid - Supplies the expected process ID.

    Uid - Supplies the expected user ID.

Return Value:

    Returns the index in the received signals array on success, -1 on failure.

--*/

{

    int Index;
    PLXT_SIGNAL_INFO Info;
    int Result;

    Info = LxtSignalFindThreadInfo();
    if (Info == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    LxtCheckResult(Index = LxtSignalCheckReceived(Signal));
    LxtCheckEqual(Signal, Info->SignalInfo[Index].si_signo, "%d");
    LxtCheckEqual(Code, Info->SignalInfo[Index].si_code, "%d");
    LxtCheckEqual(Pid, Info->SignalInfo[Index].si_pid, "%d");
    LxtCheckEqual(Uid, Info->SignalInfo[Index].si_uid, "%d");
    Result = Index;

ErrorExit:
    return Result;
}

int LxtSignalCheckNoSignal(void)

/*++

Routine Description:

    This routine checks if no signal was received.

Arguments:

    None.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    PLXT_SIGNAL_INFO Info;
    int Result;

    Info = LxtSignalFindThreadInfo();
    if (Info == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    if (Info->SignalCount == 0)
    {
        Result = LXT_RESULT_SUCCESS;
    }
    else
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Unexpected signal.");
    }

ErrorExit:
    return Result;
}

int LxtSignalCheckReceived(int Signal)

/*++

Routine Description:

    This routine checks if the specified signal was received by the signal
    handler.

Arguments:

    Signal - Supplies the expected signal number.

Return Value:

    Returns the index in the received signals array on success, -1 on failure.

--*/

{

    int Index;
    PLXT_SIGNAL_INFO Info;
    int Result;

    Info = LxtSignalFindThreadInfo();
    if (Info == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        goto SignalCheckReceivedEnd;
    }

    if (Info->SignalCount == 0)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Signal %d was not received.", Signal);
        goto SignalCheckReceivedEnd;
    }

    for (Index = 0; Index < Info->SignalCount; Index += 1)
    {
        if (Info->ReceivedSignal[Index] == -1)
        {
            Result = LXT_RESULT_FAILURE;
            LxtLogError("An error occurred in the signal handler");
            goto SignalCheckReceivedEnd;
        }

        if (Info->ReceivedSignal[Index] == Signal)
        {
            Result = Index;
            goto SignalCheckReceivedEnd;
        }
    }

    Result = LXT_RESULT_FAILURE;
    LxtLogError("Signal %d was not received!", Signal);

SignalCheckReceivedEnd:
    return Result;
}

int LxtSignalCheckSigChldReceived(int Code, pid_t Pid, uid_t Uid, int Status)

/*++

Routine Description:

    This routine checks if the SIGCHLD signal was received by the signal
    handlers, with the specified info values.

    N.B. The signal handler must have been established with SA_SIGINFO for this
         to work.

Arguments:

    Code - Supplies the expected signal code.

    Pid - Supplies the expected process ID.

    Uid - Supplies the expected user ID.

    Status - Supplies the expected process status.

Return Value:

    Returns the index in the received signals array on success, -1 on failure.

--*/

{

    int Index;
    PLXT_SIGNAL_INFO Info;
    int Result;

    Info = LxtSignalFindThreadInfo();
    if (Info == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    LxtCheckResult(Index = LxtSignalCheckInfoReceived(SIGCHLD, Code, Pid, Uid));
    LxtCheckEqual(Status, Info->SignalInfo[Index].si_status, "%d");
    Result = Index;

ErrorExit:
    return Result;
}

PLXT_SIGNAL_INFO
LxtSignalFindThreadInfo(void)

/*++

Description:

    This routine finds the signal test info for the current thread.

Arguments:

    None.

Return Value:

    A pointer to the signal info, or NULL if the signal info was not
    initialized.

--*/

{

    size_t Index;
    PLXT_SIGNAL_INFO Result;
    pid_t ThreadId;

    Result = NULL;
    ThreadId = gettid();
    for (Index = 0; Index < SIGNAL_MAX_THREADS; Index += 1)
    {
        if (g_ThreadSignalInfo[Index].ThreadId == ThreadId)
        {
            Result = &g_ThreadSignalInfo[Index];
            break;
        }
    }

    if (Result == NULL)
    {
        LxtLogError("LxtSignalInitializeThread not called for this thread.");
        goto ErrorExit;
    }

ErrorExit:
    return Result;
}

int LxtSignalGetCount(void)

/*++

Routine Description:

    This routine returns the number of received signals.

Arguments:

    None.

Return Value:

    The number of received signals, or -1 on failure.

--*/

{

    PLXT_SIGNAL_INFO Info;
    int Result;

    Info = LxtSignalFindThreadInfo();
    if (Info == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    Result = Info->SignalCount;

ErrorExit:
    return Result;
}

int LxtSignalGetInfo(siginfo_t* SignalInfo)

/*++

Routine Description:

    This routine gets a copy of the last received signal info.

Arguments:

    SignalInfo - Supplies a pointer which receives the signal info.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    PLXT_SIGNAL_INFO Info;
    int Result;

    Info = LxtSignalFindThreadInfo();
    if (Info == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    *SignalInfo = Info->SignalInfo[0];
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

void LxtSignalHandler(int Signal)

/*++

Routine Description:

    This routine handles signals for the process.

Arguments:

    Signal - Supplies the signal that was received

Return Value:

    None.

--*/

{

    int AllowedSignals;
    PLXT_SIGNAL_INFO Info;
    int Result;

    Info = NULL;

#if defined(__i386__)

    register int Eax asm("eax");
    register void* Ecx asm("ecx");
    register void* Edx asm("edx");

    //
    // Verify register contents.
    //

    LxtCheckEqual(Eax, Signal, "%d");
    LxtCheckEqual(Edx, NULL, "%p");
    LxtCheckEqual(Ecx, NULL, "%p");

    //
    // Verify stack alignment.
    //

    LxtCheckEqual((uintptr_t)&Signal & 0xf, 0, "%p");

#endif

    Info = LxtSignalFindThreadInfo();
    if (Info == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    if (Info->AllowMultipleSignals != FALSE)
    {
        AllowedSignals = SIGNAL_MAX_SIGNALS;
    }
    else
    {
        AllowedSignals = 1;
    }

    if (Info->SignalCount < AllowedSignals)
    {
        LxtLogInfo("Process %d got signal %d (%s)", getpid(), Signal, strsignal(Signal));

        Result = Signal;
    }
    else
    {
        LxtLogError("Unexpected signal %d (%s)", Signal, strsignal(Signal));
        Result = LXT_RESULT_FAILURE;
    }

ErrorExit:
    if (Info != NULL)
    {
        if (Result < 0)
        {
            Info->ReceivedSignal[0] = LXT_RESULT_FAILURE;
            Info->SignalCount = 1;
        }
        else if (Info->SignalCount < AllowedSignals)
        {
            Info->ReceivedSignal[Info->SignalCount] = Result;
            Info->SignalCount += 1;
        }
    }

    return;
}

void LxtSignalHandlerSigAction(int Signal, siginfo_t* SigInfo, void* UContext)

/*++

Routine Description:

    This routine handles signals for the process using the SA_SIGINFO flag.

Arguments:

    Signal - Supplies the signal that was received.

    SigInfo - Supplies additional information about the signal.

    UContext - Supplies the scheduling context from the process before the
        signal handler was invoked.

Return Value:

    None.

--*/

{

    int AllowedSignals;
    PLXT_SIGNAL_INFO Info;
    int Result;

    Info = NULL;

#if defined(__i386__)

    register int Eax asm("eax");
    register void* Ecx asm("ecx");
    register void* Edx asm("edx");

    //
    // Verify register contents.
    //

    LxtCheckEqual(Eax, Signal, "%d");
    LxtCheckEqual(Edx, SigInfo, "%p");
    LxtCheckEqual(Ecx, UContext, "%p");

    //
    // Verify stack alignment.
    //

    LxtCheckEqual((uintptr_t)&Signal & 0xf, 0, "%p");

#endif

    LxtCheckEqual(Signal, SigInfo->si_signo, "%d");
    Info = LxtSignalFindThreadInfo();
    if (Info == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    if (Info->AllowMultipleSignals != FALSE)
    {
        AllowedSignals = SIGNAL_MAX_SIGNALS;
    }
    else
    {
        AllowedSignals = 1;
    }

    if (Info->SignalCount < AllowedSignals)
    {
        if (Signal == SIGCHLD)
        {
            LxtLogInfo(
                "Process %d(%d) got signal %d (%s), code %d, pid %d, "
                "uid %d, status %d",
                getpid(),
                gettid(),
                SigInfo->si_signo,
                strsignal(SigInfo->si_signo),
                SigInfo->si_code,
                SigInfo->si_pid,
                SigInfo->si_uid,
                SigInfo->si_status);
        }
        else
        {
            LxtLogInfo(
                "Process %d(%d) got signal %d (%s), code %d, pid %d, uid %d",
                getpid(),
                gettid(),
                SigInfo->si_signo,
                strsignal(SigInfo->si_signo),
                SigInfo->si_code,
                SigInfo->si_pid,
                SigInfo->si_uid);
        }

        Result = Signal;
    }
    else
    {
        LxtLogError(
            "Process %d got unexpected signal %d (%s), code %d, pid %d, uid %d",
            getpid(),
            SigInfo->si_signo,
            strsignal(SigInfo->si_signo),
            SigInfo->si_code,
            SigInfo->si_pid,
            SigInfo->si_uid);

        Result = LXT_RESULT_FAILURE;
    }

ErrorExit:
    if (Info != NULL)
    {
        if (Result < 0)
        {
            Info->ReceivedSignal[0] = LXT_RESULT_FAILURE;
            Info->SignalCount = 1;
        }
        else if (Info->SignalCount < AllowedSignals)
        {
            Info->ReceivedSignal[Info->SignalCount] = Result;
            Info->SignalInfo[Info->SignalCount] = *SigInfo;
            Info->SignalCount += 1;
        }
    }

    return;
}

int LxtSignalInitialize(void)

/*++

Description:

    This routine initializes the signal test infrastructure for the current
    process.

    N.B. Run this function for any process that uses the signal test
         infrastructure. If a test uses fork(), you must run this function
         again in the child process.

Arguments:

    None.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    g_NextSignalThread = 0;
    memset(g_ThreadSignalInfo, 0, sizeof(g_ThreadSignalInfo));
    return LxtSignalInitializeThread();
}

int LxtSignalInitializeThread(void)

/*++

Description:

    This routine initializes the signal test infrastructure for the current
    thread.

    N.B. Run this function for any thread that uses the signal test
         infrastructure, except the main thread of the process; for the main
         thread, run LxtSignalInitialize instead.

Arguments:

    None.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Index;
    int Result;

    Index = __sync_fetch_and_add(&g_NextSignalThread, 1);
    if (Index >= SIGNAL_MAX_THREADS)
    {
        LxtLogError("Too many threads in signal test.");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    if (g_ThreadSignalInfo[Index].ThreadId != 0)
    {
        LxtLogError("Invalid signal test state.");
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    g_ThreadSignalInfo[Index].ThreadId = gettid();
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

void LxtSignalResetReceived(void)

/*++

Routine Description:

    This routine resets the global variables used by the signal handlers.

Arguments:

    None.

Return Value:

    None.

--*/

{

    PLXT_SIGNAL_INFO Info;

    Info = LxtSignalFindThreadInfo();
    if (Info == NULL)
    {
        goto ErrorExit;
    }

    Info->SignalCount = 0;

ErrorExit:
    return;
}

int LxtSignalSetupHandler(int Signal, int Flags)

/*++

Routine Description:

    This routine sets up a signal handler.

Arguments:

    Signal - Supplies the signal.

    Flags - Supplies the flags.

Return Value:

    0 on success, -1 on failure.

--*/

{

    struct sigaction Action;
    int Result;

    //
    // Check that the signal infrastructure was initialized properly.
    //

    if (LxtSignalFindThreadInfo() == NULL)
    {
        Result = LXT_RESULT_FAILURE;
        goto ErrorExit;
    }

    memset(&Action, 0, sizeof(Action));
    if ((Flags & SA_SIGINFO) != 0)
    {
        Action.sa_sigaction = LxtSignalHandlerSigAction;
    }
    else
    {
        Action.sa_handler = LxtSignalHandler;
    }

    Action.sa_flags = Flags;
    LxtCheckErrnoZeroSuccess(sigaction(Signal, &Action, NULL));

ErrorExit:
    return Result;
}

void LxtSignalSetAllowMultiple(BOOLEAN AllowMultiple)

/*++

Routine Description:

    This routine sets whether or not receiving another signal when one was
    already received should be not considered an error.

Arguments:

    AllowMultiple - Supplies a value that indicates whether multiple signals
        are allowed.

Return Value:

    None.

--*/

{

    PLXT_SIGNAL_INFO Info;

    Info = LxtSignalFindThreadInfo();
    if (Info == NULL)
    {
        goto ErrorExit;
    }

    Info->AllowMultipleSignals = AllowMultiple;

ErrorExit:
    return;
}

int LxtSignalTimedWait(sigset_t* Set, siginfo_t* SignalInfo, struct timespec* Timeout)

/*++

Routine Description:

    This routine calls the rt_sigtimedwait system call.

    N.B. In glibc, the sigtimedwait function is available as a wrapper for
         this system call, but in bionic only sigwait is available which
         prevents access to some of the parameters of rt_sigtimedwait.
         Even in glibc the sigtimedwait wrapper should not be used for testing
         since it silently converts SI_TKILL to SI_USER.

Arguments:

    Set - Supplies a pointer to the set of signals to wait for.

    SignalInfo - Supplies a pointer that receives information about the signal.

    Timeout - Supplies a pointer to a timeout value.

Return Value:

    The signal number on success, -1 on failure with errno set appropriately.

--*/

{

#if defined(__GLIBC__)
    sigset_t* SignalSetPointer;

    SignalSetPointer = Set;
#else
    kernel_sigset_t SignalSet;
    kernel_sigset_t* SignalSetPointer;

    //
    // Convert to the 64-bit signal set size that the kernel expects.
    //

    SignalSetPointer = NULL;
    if (Set != NULL)
    {
        SignalSet = *Set;
        SignalSetPointer = &SignalSet;
    }

#endif

    return syscall(SYS_rt_sigtimedwait, SignalSetPointer, SignalInfo, Timeout, _NSIG / 8);
}

int LxtSignalUnblock(int Signal)

/*++

Routine Description:

    This routine unblocks the specified signal.

Arguments:

    Signal - Supplies the signal number.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    sigset_t Signals;

    sigemptyset(&Signals);
    sigaddset(&Signals, Signal);
    LxtCheckErrnoZeroSuccess(sigprocmask(SIG_UNBLOCK, &Signals, NULL));

ErrorExit:
    return Result;
}

void LxtSignalWait(void)

/*++

Routine Description:

    This routine waits until a signal has been received, or a timeout expires.

    N.B. This function does not return status to indicate whether a signal was
         received or not. Use the signal check functions after this
         function returns.

Arguments:

    None.

Return Value:

    None.

--*/

{

    PLXT_SIGNAL_INFO Info;
    int WaitCount;

    Info = LxtSignalFindThreadInfo();
    if (Info == NULL)
    {
        goto ErrorExit;
    }

    //
    // N.B. It would be possible to implement this function using sigsuspend
    //      but only after signal blocking is implemented. In order to avoid
    //      a race where sigsuspend might hang if the signal arrives before
    //      the call, the relevant signal should be blocked before doing the
    //      operation that generates the signal, then call sigsuspend with a
    //      mask that unblocks the signal.
    //

    for (WaitCount = 0; (WaitCount < SIGNAL_WAIT_COUNT) && (Info->SignalCount == 0); WaitCount += 1)
    {

        usleep(SIGNAL_WAIT_TIMEOUT_US);
    }

ErrorExit:
    return;
}

int LxtSignalWaitBlocked(int Signal, pid_t FromPid, int TimeoutSeconds)

/*++

Routine Description:

    This routine waits for a specific blocked signal.

Arguments:

    Signal - Supplies the signal number.

    FromPid - Supplies the expected origin of the signal.

    TimeoutSeconds - Supplies the timeout, in seconds.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int ReceivedSignal;
    int Result;
    siginfo_t SignalInfo;
    sigset_t Signals;
    struct timespec Timeout;

    sigemptyset(&Signals);
    sigaddset(&Signals, Signal);
    Timeout.tv_sec = TimeoutSeconds;
    Timeout.tv_nsec = 0;
    LxtCheckErrno(ReceivedSignal = LxtSignalTimedWait(&Signals, &SignalInfo, &Timeout));

    LxtCheckEqual(Signal, ReceivedSignal, "%d");
    LxtCheckEqual(SignalInfo.si_pid, FromPid, "%d");

ErrorExit:
    return Result;
}

int LxtSocketPairClose(PLXT_SOCKET_PAIR SocketPair)

/*++

Routine Description:

    This routine closes a socket pair.

Arguments:

    SocketPair - Supplies a pointer to the socket pair.

Return Value:

    0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckResult(LxtSocketPairCloseChild(SocketPair));
    LxtCheckResult(LxtSocketPairCloseParent(SocketPair));

ErrorExit:
    return Result;
}

int LxtSocketPairCloseChild(PLXT_SOCKET_PAIR SocketPair)

/*++

Routine Description:

    This routine closes the child socket of a socket pair.

Arguments:

    SocketPair - Supplies a pointer to the socket pair.

Return Value:

    0 on success, -1 on failure.

--*/

{

    int Result;

    if (SocketPair->Child != 0)
    {
        LxtCheckErrnoZeroSuccess(close(SocketPair->Child));
        SocketPair->Child = 0;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int LxtSocketPairCloseParent(PLXT_SOCKET_PAIR SocketPair)

/*++

Routine Description:

    This routine closes the parent socket of a socket pair.

Arguments:

    SocketPair - Supplies a pointer to the socket pair.

Return Value:

    0 on success, -1 on failure.

--*/

{

    int Result;

    if (SocketPair->Parent != 0)
    {
        LxtCheckErrnoZeroSuccess(close(SocketPair->Parent));
        SocketPair->Parent = 0;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int LxtSocketPairCreate(PLXT_SOCKET_PAIR SocketPair)

/*++

Routine Description:

    This routine creates a socket pair.

Arguments:

    SocketPair - Supplies a pointer to the socket pair.

Return Value:

    0 on success, -1 on failure.

--*/

{

    int Result;

    memset(SocketPair, 0, sizeof(*SocketPair));
    LxtCheckErrnoZeroSuccess(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, (int*)SocketPair));

ErrorExit:
    return Result;
}

int LxtWaitPidPoll(pid_t ChildPid, int ExpectedWaitStatus)

/*++

Routine Description:

    This routine waits until the specified child exits by polling its wait
    status repeatedly.

Arguments:

    ChildPid - Supplies the thread group ID of the child to wait on.

    ExpectedWaitStatus - Supplies the expected value of the child's status.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    return LxtWaitPidPollOptions(ChildPid, ExpectedWaitStatus, 0, LXT_WAITPID_DEFAULT_TIMEOUT);
}

int LxtWaitPidPollOptions(pid_t ChildPid, int ExpectedWaitStatus, int Options, int TimeoutSeconds)

/*++

Routine Description:

    This routine waits until the specified child exits by polling its wait
    status repeatedly.

Arguments:

    ChildPid - Supplies the thread group ID of the child to wait on.

    ExpectedWaitStatus - Supplies the expected value of the child's status.

    Options - Supplies wait options to pass to waitpid.

    TimeoutSeconds - Supplies the number of seconds to wait for the child.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int SecondWaitPidStatus;
    int Result;
    int WaitCount;
    int WaitCountTotal;
    int WaitPidResult;
    int WaitPidStatus;

    //
    // Only WNOHANG is supported right now, so poll for the result and check the
    // status.
    //

    Options |= WNOHANG;
    WaitCountTotal = (TimeoutSeconds * 1000000) / LXT_WAITPID_WAIT_TIMEOUT_US;
    for (WaitCount = 0; WaitCount < WaitCountTotal; ++WaitCount)
    {
        LxtCheckErrno((WaitPidResult = waitpid(ChildPid, &WaitPidStatus, Options)));

        if (WaitPidResult != 0)
        {
            if ((WaitPidStatus & 0x80000000) != 0)
            {
                Result = LXT_RESULT_FAILURE;
                LxtLogError("Unexpected high bit: %x - %x", WaitPidStatus, ExpectedWaitStatus);

                goto ErrorExit;
            }

            if (WaitPidStatus != ExpectedWaitStatus)
            {
                Result = LXT_RESULT_FAILURE;
                LxtLogError("Unexpected status: %x != %x", WaitPidStatus, ExpectedWaitStatus);

                goto ErrorExit;
            }

            if (WIFEXITED(WaitPidStatus) != 0)
            {
                LxtCheckErrnoFailure(waitpid(ChildPid, &SecondWaitPidStatus, WNOHANG), ECHILD);
            }

            Result = WaitPidResult;
            break;
        }

        usleep(LXT_WAITPID_WAIT_TIMEOUT_US);
    }

    if (WaitCount == WaitCountTotal)
    {
        Result = LXT_RESULT_FAILURE;
        LxtLogError("Failed to receive status %d from child {:%d:}", ExpectedWaitStatus, ChildPid);

        goto ErrorExit;
    }

ErrorExit:
    return Result;
}

int LxtClose(int FileDescriptor)

/*++
--*/

{
    int Result;

    LxtCheckErrnoZeroSuccess(close(FileDescriptor));

ErrorExit:
    return Result;
}

int LxtMunmap(void* Address, size_t Length)

/*++
--*/

{
    int Result;

    LxtCheckErrno(munmap(Address, Length));

ErrorExit:
    return Result;
}

int LxtWslVersion(void)

/*++

Description:

    This routine determines whether the tests are running in WSL1 or 2.

Arguments:

    None.

Return Value:

    The WSL version number, 1 or 2, or 0 if an error occurred.

--*/

{

    int Result;
    struct utsname UnameBuffer;

    if (g_WslVersion == 0)
    {
        memset(&UnameBuffer, 0, sizeof(UnameBuffer));
        LxtCheckErrno(uname(&UnameBuffer));
        if (strstr(UnameBuffer.release, "Microsoft") == NULL)
        {
            g_WslVersion = 2;
        }
        else
        {
            g_WslVersion = 1;
        }
    }

ErrorExit:
    return g_WslVersion;
}