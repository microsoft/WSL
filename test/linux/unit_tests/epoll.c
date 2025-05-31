/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    epoll.c

Abstract:

    This file is the epoll test.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netdb.h>
#include "common.h"

#include <sys/wait.h>

#define LXT_NAME "Epoll"
#define SOCKET_NAME "PartyInTheUsa"
#define EPOLL_DUP2_FD_COUNT 100

typedef struct _EPOLL_DUP2_CONTEXT
{
    int EpollFd;
    int Fd[EPOLL_DUP2_FD_COUNT];
} EPOLL_DUP2_CONTEXT, *PEPOLL_DUP2_CONTEXT;

LXT_VARIATION_HANDLER EpollAddTest;
LXT_VARIATION_HANDLER EpollBasic;

int EpollBasicVariation(unsigned short ReadFlags, unsigned short WriteFlags);

LXT_VARIATION_HANDLER EpollDeleteCloseFdLoop;
LXT_VARIATION_HANDLER EpollDeleteTest;
LXT_VARIATION_HANDLER EpollDup2FdLoop;
LXT_VARIATION_HANDLER EpollHangupTestSimple;
LXT_VARIATION_HANDLER EpollHangupTestUnix;
LXT_VARIATION_HANDLER PPollInvalidArgument;
LXT_VARIATION_HANDLER EpollModifyWhilePollingTest;
LXT_VARIATION_HANDLER EpollModTest;
LXT_VARIATION_HANDLER EpollPhantomEventsTest;
LXT_VARIATION_HANDLER EpollRecursionTest;
LXT_VARIATION_HANDLER EpollRecursionLimitTest;
LXT_VARIATION_HANDLER EpollRelatedFileStress;
LXT_VARIATION_HANDLER EpollSequenceTestUnix;
LXT_VARIATION_HANDLER EpollSocketAcceptTest;
LXT_VARIATION_HANDLER EpollSocketReadTest;
LXT_VARIATION_HANDLER EpollUnalignedTest;
LXT_VARIATION_HANDLER EpollVariation0;

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Basic_Variations", EpollBasic},
    {"Epoll", EpollVariation0},
    {"Epoll_Read", EpollSocketReadTest},
    {"Epoll_Hangup", EpollHangupTestSimple},
    {"Epoll_Accept", EpollSocketAcceptTest},
    {"Epoll_Add", EpollAddTest},
    {"Epoll_Delete", EpollDeleteTest},
    {"Epoll_Modify_WhilePolling", EpollModifyWhilePollingTest},
    {"Epoll_Related_File_Stress", EpollRelatedFileStress},
    {"Epoll_Mod", EpollModTest},
    {"Epoll_PhantomEvents", EpollPhantomEventsTest},
    {"Ppoll invalid argument", PPollInvalidArgument},
    {"Epoll unaligned", EpollUnalignedTest},
    {"Epoll delete, close FD loop", EpollDeleteCloseFdLoop},
    {"Epoll dup2 FD loop", EpollDup2FdLoop},
    {"Epoll basic recursion", EpollRecursionTest},
    {"Epoll recursion limit", EpollRecursionLimitTest}};

int EpollTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Result;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LXT_SYNCHRONIZATION_POINT_INIT();
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_DESTROY();
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

#ifndef EPOLLONESHOT
#define EPOLLONESHOT (1 << 30)
#endif

int EpollBasic(PLXT_ARGS Args)

/*++
--*/

{
    unsigned short ReadFlags[] = {EPOLLIN, EPOLLRDNORM, (EPOLLIN | EPOLLRDNORM)};
    int ReadVariation;
    int Result;
    unsigned short WriteFlags[] = {EPOLLOUT, EPOLLWRNORM, (EPOLLOUT | EPOLLWRNORM)};
    int WriteVariation;

    for (ReadVariation = 0; ReadVariation < LXT_COUNT_OF(ReadFlags); ReadVariation += 1)
    {

        for (WriteVariation = 0; WriteVariation < LXT_COUNT_OF(ReadFlags); WriteVariation += 1)
        {

            Result = EpollBasicVariation(ReadFlags[ReadVariation], WriteFlags[WriteVariation]);

            if (Result < 0)
            {
                LxtLogError("Failed basic variation (%d, %d)", ReadVariation, WriteVariation);

                goto cleanup;
            }
        }
    }

cleanup:
    return Result;
}

int EpollBasicVariation(unsigned short ReadFlags, unsigned short WriteFlags)

/*++
--*/

{

    struct epoll_event EpollControlEvent;
    int EpollFileDescriptor;
    struct epoll_event EpollWaitEvent[2];
    struct epoll_event* InputEvent;
    struct epoll_event* OutputEvent;
    int PipeFileDescriptors[2] = {};
    int Result;

    //
    // Initialize locals.
    //

    EpollFileDescriptor = -1;

    //
    // Open a pipe to test epoll.
    //

    LxtCheckErrnoZeroSuccess(pipe(PipeFileDescriptors));

    //
    // Pend a write.
    //

    LxtCheckErrno(write(PipeFileDescriptors[1], "\n", 1));

    //
    // Create an epoll.
    //

    LxtCheckErrno(EpollFileDescriptor = epoll_create(1));

    //
    // Add the file to the epoll.
    //

    EpollControlEvent.events = ReadFlags;
    EpollControlEvent.data.fd = PipeFileDescriptors[0];
    LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, PipeFileDescriptors[0], &EpollControlEvent));

    EpollControlEvent.events = WriteFlags | EPOLLPRI;
    EpollControlEvent.data.fd = PipeFileDescriptors[1];
    LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, PipeFileDescriptors[1], &EpollControlEvent));

    //
    // Verify the epoll is triggered.
    //

    LxtCheckErrno(epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, 0));
    if (Result != 2)
    {
        LxtLogError("Waiting on epoll returned %d events (expecting 2)!", Result);
        Result = -1;
        goto ErrorExit;
    }

    if (EpollWaitEvent[0].data.fd == PipeFileDescriptors[1])
    {
        InputEvent = &EpollWaitEvent[1];
        OutputEvent = &EpollWaitEvent[0];
    }
    else
    {
        InputEvent = &EpollWaitEvent[0];
        OutputEvent = &EpollWaitEvent[1];
    }

    LxtCheckEqual(InputEvent->data.fd, PipeFileDescriptors[0], "%d");
    LxtCheckEqual(InputEvent->events, ReadFlags, "%hd");
    LxtCheckEqual(OutputEvent->data.fd, PipeFileDescriptors[1], "%d");
    LxtCheckEqual(OutputEvent->events, WriteFlags, "%hd");

ErrorExit:
    if (EpollFileDescriptor != -1)
    {
        close(EpollFileDescriptor);
    }

    if (PipeFileDescriptors[1] != -1)
    {
        close(PipeFileDescriptors[1]);
    }

    if (PipeFileDescriptors[0] != -1)
    {
        close(PipeFileDescriptors[0]);
    }

    return Result;
}

int EpollCreateClientSocket(void)

/*++
--*/

{
    int Result;
    struct sockaddr_in ServerAddress = {0};
    int Socket;

    //
    // Create a socket.
    //

    Socket = socket(AF_INET, SOCK_STREAM, 0);
    if (Socket < 0)
    {
        LxtLogError("socket(AF_INET, SOCK_STREAM, 0) - %s", strerror(errno));
        Result = -1;
        goto cleanup;
    }

    //
    // Connect to the server.
    //

    ServerAddress.sin_family = AF_INET;
    ServerAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ServerAddress.sin_port = htons(LXT_SOCKET_DEFAULT_PORT);

    Result = connect(Socket, (struct sockaddr*)&ServerAddress, sizeof(ServerAddress));

    if (Result < 0)
    {
        LxtLogError("connect(%d) - %s", Socket, strerror(errno));
        Result = -1;
        goto cleanup;
    }

    Result = Socket;
    Socket = 0;

cleanup:
    if (Socket > 0)
    {
        if (close(Socket) != 0)
        {
            LxtLogError("close(%d) - %s", Socket, strerror(errno));
            Result = LXT_RESULT_FAILURE;
        }
    }

    return Result;
}

int EpollCreateClientUnixSocket(int SocketType)

/*++
--*/

{
    int Result;
    struct sockaddr_un ServerAddress = {0};
    int Socket;

    //
    // Create a socket.
    //

    Socket = socket(AF_UNIX, SocketType, 0);
    if (Socket < 0)
    {
        LxtLogError("socket(AF_UNIX, SocketType, 0) - %s", strerror(errno));
        Result = -1;
        goto cleanup;
    }

    //
    // Connect to the server.
    //

    ServerAddress.sun_family = AF_UNIX;
    strcpy(ServerAddress.sun_path, SOCKET_NAME);

    Result = connect(Socket, (struct sockaddr*)&ServerAddress, sizeof(ServerAddress));

    if (Result < 0)
    {
        LxtLogError("connect(%d) - %s", Socket, strerror(errno));
        Result = -1;
        goto cleanup;
    }

    Result = Socket;
    Socket = 0;

cleanup:
    if (Socket > 0)
    {
        if (close(Socket) != 0)
        {
            LxtLogError("close(%d) - %s", Socket, strerror(errno));
            Result = LXT_RESULT_FAILURE;
        }
    }

    return Result;
}

int EpollCreateListenSocket(void)

/*++
--*/

{

    struct sockaddr_in ServerAddress = {0};
    int Result;
    int Socket;

    //
    // Create a socket.
    //

    Socket = socket(AF_INET, SOCK_STREAM, 0);
    if (Socket < 0)
    {
        LxtLogError("socket - %s", strerror(errno));
        Result = -1;
        goto cleanup;
    }

    //
    // Bind the socket to an ipv4 socket.
    //

    ServerAddress.sin_family = AF_INET;
    ServerAddress.sin_addr.s_addr = INADDR_ANY;
    ServerAddress.sin_port = htons(LXT_SOCKET_DEFAULT_PORT);

    Result = bind(Socket, (struct sockaddr*)&ServerAddress, sizeof(ServerAddress));

    if (Result < 0)
    {
        LxtLogError("bind(%d) - %s", Socket, strerror(errno));
        Result = -1;
        goto cleanup;
    }

    //
    // Mark the socket as a listen socket.
    //

    Result = listen(Socket, LXT_SOCKET_SERVER_MAX_BACKLOG_NUM);
    if (Result < 0)
    {
        LxtLogError("listen(%d) - %s", Socket, strerror(errno));
        Result = -1;
        goto cleanup;
    }

    Result = Socket;
    Socket = 0;

cleanup:

    if (Socket > 0)
    {
        if (close(Socket) != 0)
        {
            LxtLogError("close(%d) - %s", Socket, strerror(errno));
        }
    }

    return Result;
}

int EpollCreateListenUnixSocket(int SocketType)

/*++
--*/

{

    struct sockaddr_un ServerAddress = {0};
    int Result;
    int Socket;

    //
    // Create a socket.
    //

    Socket = socket(AF_UNIX, SocketType, 0);
    if (Socket < 0)
    {
        LxtLogError("socket - %s", strerror(errno));
        Result = -1;
        goto cleanup;
    }

    //
    // Bind the socket to an ipv4 socket.
    //

    ServerAddress.sun_family = AF_UNIX;
    strcpy(ServerAddress.sun_path, SOCKET_NAME);
    unlink(SOCKET_NAME);
    Result = bind(Socket, (struct sockaddr*)&ServerAddress, sizeof(ServerAddress));

    if (Result < 0)
    {
        LxtLogError("bind(%d) - %s", Socket, strerror(errno));
        Result = -1;
        goto cleanup;
    }

    //
    // Mark the socket as a listen socket.
    //

    Result = listen(Socket, LXT_SOCKET_SERVER_MAX_BACKLOG_NUM);
    if (Result < 0)
    {
        LxtLogError("listen(%d) - %s", Socket, strerror(errno));
        Result = -1;
        goto cleanup;
    }

    Result = Socket;
    Socket = 0;

cleanup:

    if (Socket > 0)
    {
        if (close(Socket) != 0)
        {
            LxtLogError("close(%d) - %s", Socket, strerror(errno));
        }
    }

    return Result;
}
int EpollHandleClientAccept(int Socket)

/*++
--*/

{

    struct sockaddr_in ClientAddress = {0};
    socklen_t ClientLength;
    int ClientSocket;

    ClientLength = sizeof(ClientAddress);
    ClientSocket = accept(Socket, (struct sockaddr*)&ClientAddress, &ClientLength);
    if (ClientSocket < 0)
    {
        LxtLogError("accept(%d) - %s", Socket, strerror(errno));
        ClientSocket = -1;
        goto cleanup;
    }

cleanup:

    return ClientSocket;
}

const char* DataToWrite[] = {
    "<This is the first message> ",
    "<This is another message> ",
    "<Dumbledore is dead> ",
    "<Harry Potter must not go back to Hogwarts> ",
    "<There must always be a stark in Winterfell>",
};

const int WriteItemCount = sizeof(DataToWrite) / sizeof(DataToWrite[0]);

int EpollSocketReadTest(PLXT_ARGS Args)

/*++
--*/

{

    char Buffer[256];
    int Result;
    int EpollFileDescriptor;
    int FileDescriptor1;
    int FileDescriptor2;
    struct epoll_event EpollControlEvent;
    struct epoll_event EpollWaitEvent[2];
    int ChildPid;
    int Index;
    int ChildStatus;

    //
    // Initialize locals.
    //

    FileDescriptor1 = -1;
    FileDescriptor2 = -1;
    EpollFileDescriptor = -1;
    ChildPid = -1;

    //
    // Create the server socket.
    //

    LxtLogInfo("[Setup] About to create server socket...");

    FileDescriptor1 = EpollCreateListenSocket();
    if (FileDescriptor1 == -1)
    {
        Result = errno;
        LxtLogError("[Setup] Could not create socket! %d", Result);
        goto cleanup;
    }

    //
    // Fork to create a server and a client.
    //

    LxtLogInfo("[Setup] About to fork...");

    ChildPid = fork();

    if (ChildPid == -1)
    {
        Result = errno;
        LxtLogError("[Setup] Fork failed! %d", Result);
        goto cleanup;
    }

    if (ChildPid == 0)
    {

        LxtLogInfo("[Client] Waiting 2 seconds to let server block...");

        usleep(2 * 1000 * 1000);

        LxtLogInfo("[Client] Connecting to server...");

        FileDescriptor2 = EpollCreateClientSocket();

        LxtLogInfo("[Client] Connected to server, fd = %d", FileDescriptor2);

        LxtLogInfo("[Client] Sleeping for 5 seconds with open socket");

        usleep(5 * 1000 * 1000);

        //
        // Create an epoll container.
        //

        EpollFileDescriptor = epoll_create(1);

        if (EpollFileDescriptor == -1)
        {
            Result = errno;
            LxtLogError("[Client] Could not create Epoll! %d", Result);
            goto cleanup;
        }

        //
        // Add the connected socket to the epoll.
        //

        EpollControlEvent.events = EPOLLIN;
        EpollControlEvent.data.fd = FileDescriptor2;

        Result = epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, FileDescriptor2, &EpollControlEvent);

        if (Result == -1)
        {
            Result = errno;
            LxtLogError("[Client] Could not add file to epoll! %d", Result);
            goto cleanup;
        }

        //
        // Wait for data to be available with a timeout.
        //

        while (1)
        {

            LxtLogInfo("[Client] Waiting on epoll with 15 second timeout ...");

            Result = epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, 15000);

            LxtLogInfo("[Client] Epoll returned %d events", Result);

            if (Result != 1)
            {
                LxtLogError("[Client] Wait on epoll failed! %d", Result);
                Result = -1;
                goto cleanup;
            }

            LxtLogInfo("[Client] Event: {%d, %x} ", EpollWaitEvent[0].data.fd, EpollWaitEvent[0].events);

            if (EpollWaitEvent[0].data.fd != FileDescriptor2)
            {
                LxtLogError("[Client] Epoll wait satisfied with wrong user data! %d", EpollWaitEvent[0].data.fd);
                Result = -1;
                goto cleanup;
            }

            if (EpollWaitEvent[0].events != EPOLLIN)
            {
                LxtLogError("[Client] Epoll wait satisfied with wrong events! 0x%x", EpollWaitEvent[0].events);
                Result = -1;
                goto cleanup;
            }

            memset(Buffer, 0, sizeof(Buffer));
            Result = read(FileDescriptor2, Buffer, sizeof(Buffer));
            if (Result < 0)
            {
                Result = -1;
                LxtLogError("[Client] Read on socket failed! %d", Result);
                goto cleanup;
            }

            LxtLogInfo("[Client] read %d bytes: %s ...", Result, Buffer);

            if (Result == 0)
            {
                LxtLogInfo("[Client] exiting ...");
                goto cleanup;
            }
        }
    }

    //
    // Accept an incoming connection.
    //

    FileDescriptor2 = EpollHandleClientAccept(FileDescriptor1);

    LxtLogInfo("[Server] Writing to socket %d times!", WriteItemCount);

    for (Index = 0; Index < WriteItemCount; Index += 1)
    {

        Result = write(FileDescriptor2, DataToWrite[Index], strlen(DataToWrite[Index]));

        if (Result < 0)
        {
            LxtLogError("[Server] Write %d failed %d", Index, Result);

            goto cleanup;
        }

        LxtLogInfo("[Server] Write (%d, %s, %d) -> %d!", FileDescriptor2, DataToWrite[Index], strlen(DataToWrite[Index]) + (Index == WriteItemCount - 1), Result);

        if ((Index % 5) == 0)
        {
            usleep(5 * 1000 * 1000);
        }
    }

    usleep(5 * 1000 * 1000);

    LxtLogInfo("[Server] Closing client fd = %d", FileDescriptor2);
    if (FileDescriptor2 != -1)
    {
        close(FileDescriptor2);
        FileDescriptor2 = -1;
    }

    LxtLogInfo("[Server] Waiting for child to exit");

    ChildStatus = 0;
    wait(&ChildStatus);

    LxtLogInfo("[Server] Child WIFEXITED=%d WEXITSTATUS=%d", WIFEXITED(ChildStatus), WEXITSTATUS(ChildStatus));

    //
    // Determine if the test passed or failed.
    //

    if ((Result < 0) || (WIFEXITED(ChildStatus) == 0) || (WEXITSTATUS(ChildStatus) != 0))
    {

        LxtLogInfo("[Server] Test failed!");
        Result = -1;
    }

    LxtLogInfo("[Server] Done");

cleanup:

    if (FileDescriptor1 != -1)
    {
        close(FileDescriptor1);
    }

    if (EpollFileDescriptor != -1)
    {
        close(EpollFileDescriptor);
    }

    if (FileDescriptor2 != -1)
    {
        close(FileDescriptor2);
    }

    if (ChildPid == 0)
    {
        LxtLogInfo("[Child] Exit with %d!", Result);
        _exit(Result);
    }

    return Result;
}

int EpollHangupTestSimple(PLXT_ARGS Args)
{

    char Buffer[256];
    int Result;
    int EpollFileDescriptor;
    int FileDescriptor1;
    int FileDescriptor2;
    struct epoll_event EpollControlEvent;
    struct epoll_event EpollWaitEvent[2];
    int ChildPid;
    int Index;
    int ChildStatus;
    int ReadAttempts;

    //
    // Initialize locals.
    //

    FileDescriptor1 = -1;
    FileDescriptor2 = -1;
    EpollFileDescriptor = -1;
    ChildPid = -1;
    LxtLogInfo("[Setup] Starting simple hangup test");

    //
    // Create the server socket.
    //

    LxtLogInfo("[Setup] About to create server socket");

    FileDescriptor1 = EpollCreateListenSocket();
    if (FileDescriptor1 == -1)
    {
        Result = errno;
        LxtLogError("[Setup] Could not create server socket %d!", Result);
        goto cleanup;
    }

    LxtLogInfo("[Setup] Created server socket successfully");

    //
    // Fork to create a server and a client.
    //

    LxtLogInfo("[Setup] About to fork");

    ChildPid = fork();

    if (ChildPid == -1)
    {
        Result = errno;
        LxtLogError("[Setup] Fork failed! %d", Result);
        goto cleanup;
    }

    if (ChildPid == 0)
    {

        LxtLogInfo("[Client] Connecting to server...");

        FileDescriptor2 = EpollCreateClientSocket();

        LxtLogInfo("[Client] Connected to server, fd = %d", FileDescriptor2);

        LxtLogInfo("[Client] Sleeping for 3 seconds with open socket");

        usleep(3 * 1000 * 1000);

        //
        // Create an epoll container.
        //

        EpollFileDescriptor = epoll_create(1);

        if (EpollFileDescriptor == -1)
        {
            Result = errno;
            LxtLogError("[Client] Could not create Epoll %d!", Result);
            goto cleanup;
        }

        //
        // Add the connected socket to the epoll.
        //

        EpollControlEvent.events = EPOLLIN;
        EpollControlEvent.data.fd = FileDescriptor2;

        Result = epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, FileDescriptor2, &EpollControlEvent);

        if (Result == -1)
        {
            Result = errno;
            LxtLogError("[Client] Could not add file to epoll %d!", Result);
            goto cleanup;
        }

        //
        // Wait for data to be available with a timeout.
        //

        ReadAttempts = 0;
        while (1)
        {

            LxtLogInfo("[Client] Waiting on epoll with 15 second timeout");

            Result = epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, 15000);

            LxtLogInfo("[Client] Epoll returned %d events", Result);

            if (Result == 0)
            {
                LxtLogError("[Client] No events returned, exiting!");
                Result = -1;
                goto cleanup;
            }

            if (Result != 1)
            {
                LxtLogError("[Client] Wait on epoll returned too many events, exiting!");
                Result = -1;
                goto cleanup;
            }

            LxtLogInfo("[Client] Event: {%d, %x} ", EpollWaitEvent[0].data.fd, EpollWaitEvent[0].events);

            if (EpollWaitEvent[0].data.fd != FileDescriptor2)
            {
                LxtLogError("[Client] Epoll wait satisfied with wrong user data! %d", EpollWaitEvent[0].data.fd);
                Result = -1;
                goto cleanup;
            }

            if (EpollWaitEvent[0].events != EPOLLIN)
            {
                LxtLogError("[Client] Epoll wait satisfied with wrong events! 0x%x", EpollWaitEvent[0].events);
                Result = -1;
                goto cleanup;
            }

            memset(Buffer, 0, sizeof(Buffer));
            Result = read(FileDescriptor2, Buffer, sizeof(Buffer));
            if (Result < 0)
            {
                Result = errno;
                LxtLogError("[Client] Read on socket failed! %d", Result);
                goto cleanup;
            }

            LxtLogInfo("[Client] Read (%d) -> %d bytes: %s", FileDescriptor2, Result, Buffer);

            if (Result == 0)
            {
                ReadAttempts += 1;
                if (ReadAttempts < 2)
                {
                    LxtLogInfo("[Client] Continuing even through read 0 bytes.");
                    continue;
                }

                LxtLogInfo("[Client] Exiting because read returned 0 bytes.");
                goto cleanup;
            }
        }
    }

    //
    // Accept an incoming connection.
    //

    LxtLogInfo("[Server] Waiting for incoming connections...");

    FileDescriptor2 = EpollHandleClientAccept(FileDescriptor1);

    LxtLogInfo("[Server] Connected to client, fd = %d", FileDescriptor2);

    Result = write(FileDescriptor2, "Party On", strlen("Party On") + 1);

    LxtLogInfo("[Server] Write (%d, %s, %d) -> %d", FileDescriptor2, "Party On", strlen("Party On") + 1, Result);

    if (Result < 0)
    {
        LxtLogError("[Server] Write failed %s", strerror(errno));
        goto cleanup;
    }

    LxtLogInfo("[Server] Closing client fd = %d", FileDescriptor2);
    if (FileDescriptor2 != -1)
    {
        close(FileDescriptor2);
        FileDescriptor2 = -1;
    }

    LxtLogInfo("[Server] Waiting for child to exit");

    wait(&ChildStatus);

    LxtLogInfo("[Server] Child WIFEXITED=%d WEXITSTATUS=%d", WIFEXITED(ChildStatus), WEXITSTATUS(ChildStatus));

    //
    // Determine if the test passed or failed.
    //

    if ((Result < 0) || (WIFEXITED(ChildStatus) == 0) || (WEXITSTATUS(ChildStatus) != 0))
    {

        LxtLogInfo("[Server] Test failed!");
        Result = -1;
    }

    LxtLogInfo("[Server] Done");

cleanup:

    if (FileDescriptor1 != -1)
    {
        close(FileDescriptor1);
    }

    if (EpollFileDescriptor != -1)
    {
        close(EpollFileDescriptor);
    }

    if (FileDescriptor2 != -1)
    {
        close(FileDescriptor2);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int EpollSocketAcceptTest(PLXT_ARGS Args)

/*++
--*/

{

    char Buffer[256];
    int Result;
    int EpollFileDescriptor;
    int FileDescriptor1;
    int FileDescriptor2;
    struct epoll_event EpollControlEvent;
    struct epoll_event EpollWaitEvent[2];
    int ChildPid;
    int Index;
    int ChildStatus;

    //
    // Initialize locals.
    //

    FileDescriptor1 = -1;
    FileDescriptor2 = -1;
    EpollFileDescriptor = -1;
    ChildPid = -1;

    //
    // Create a socket that will be added for epoll.
    //

    FileDescriptor1 = EpollCreateListenSocket();
    if (FileDescriptor1 == -1)
    {
        Result = errno;
        LxtLogError("Could not create socket! %d", Result);
        goto cleanup;
    }

    //
    // Create an epoll container.
    //

    EpollFileDescriptor = epoll_create(1);

    if (EpollFileDescriptor == -1)
    {
        Result = errno;
        LxtLogError("Could not create Epoll! %d", Result);
        goto cleanup;
    }

    //
    // Add the socket to the epoll.
    //

    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = FileDescriptor1;

    Result = epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, FileDescriptor1, &EpollControlEvent);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Could not add file to epoll! %d", Result);
        goto cleanup;
    }

    //
    // Wait for data to be available with a timeout. No data should arrive.
    //

    LxtLogInfo("[Setup] Waiting on epoll to timeout for 5s...");

    Result = epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, 5000);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Waiting on epoll failed! %d", Result);
        goto cleanup;
    }

    if (Result != 0)
    {
        LxtLogError("Waiting on epoll succeeded but returned non-zero events! %d", Result);
        Result = -1;
        goto cleanup;
    }

    LxtLogInfo("[Setup] Wait on epoll returned no data, as expected...");

    //
    // Fork to create a server and a client.
    //

    LxtLogInfo("[Setup] About to fork...");

    ChildPid = fork();

    if (ChildPid == -1)
    {
        Result = errno;
        LxtLogError("Fork failed! %d", Result);
        goto cleanup;
    }

    if (ChildPid == 0)
    {

        LxtLogInfo("[Client] Waiting 2 seconds to let server block...");

        usleep(2 * 1000 * 1000);

        LxtLogInfo("[Client] Connecting to server...");

        FileDescriptor2 = EpollCreateClientSocket();

        LxtLogInfo("[Client] Connected to server, fd =%d", FileDescriptor2);

        LxtLogInfo("[Client] Sleeping for 2 seconds with open socket");

        usleep(2 * 1000 * 1000);

        Result = write(FileDescriptor2, "Party On", strlen("Party On") + 1);

        LxtLogInfo("[Client] Write (%d, %s, %d) -> %d", FileDescriptor2, "Party On", strlen("Party On") + 1, Result);

        if (Result < 0)
        {
            LxtLogError("[Server] Write failed %s", strerror(errno));
            goto cleanup;
        }

        LxtLogInfo("[Client] Closing socket %d", FileDescriptor2);

        if (close(FileDescriptor2) != 0)
        {
            LxtLogError("[Client] Closing socket %d failed - %s", FileDescriptor2, strerror(errno));
        }

        usleep(2 * 1000 * 1000);

        FileDescriptor2 = -1;
        goto cleanup;
    }

    //
    // The server should wait for data to become available on the socket.
    //

    LxtLogInfo("[Server] Waiting on epoll to timeout for 10s...");

    Result = epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, 10000);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("[Server] Waiting on epoll failed! %d", Result);
        goto cleanup;
    }

    if (Result != 1)
    {
        LxtLogError("[Server] Waiting on epoll returned unexpected events: %d!", Result);
        Result = -1;
        goto cleanup;
    }

    if (EpollWaitEvent[0].data.fd != FileDescriptor1)
    {
        LxtLogError("[Server] Epoll wait satisfied with wrong user data! %d", EpollWaitEvent[0].data.fd);
        Result = -1;
        goto cleanup;
    }

    if (EpollWaitEvent[0].events != EPOLLIN)
    {
        LxtLogError("[Server] Epoll wait satisfied with wrong events! 0x%x", EpollWaitEvent[0].events);
        Result = -1;
        goto cleanup;
    }

    FileDescriptor2 = EpollHandleClientAccept(FileDescriptor1);
    if (FileDescriptor2 < 0)
    {
        LxtLogError("[Server] Accept failed!");
        Result = -1;
    }

    LxtLogInfo("[Server] Accepted a request successfully...");

    //
    // The server should timeout now if it waits for data again.
    //

    LxtLogInfo("[Server] Waiting on epoll to timeout for 5s...");

    Result = epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, 5000);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Waiting on epoll failed! %d", Result);
        goto cleanup;
    }

    if (Result != 0)
    {
        LxtLogError("Waiting on epoll succeeded but returned non-zero events! %d", Result);
        Result = -1;
        goto cleanup;
    }

    //
    // Add the socket to the epoll.
    //

    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = FileDescriptor2;

    Result = epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, FileDescriptor2, &EpollControlEvent);

    if (Result == -1)
    {
        Result = errno;
        LxtLogError("Could not add file to epoll! %d", Result);
        goto cleanup;
    }

    //
    // Wait for data to be available with a timeout. No data should arrive.
    //

    while (1)
    {

        LxtLogInfo("[Setup] Waiting on epoll to timeout for 5s...");

        Result = epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, 5000);

        if (Result == -1)
        {
            Result = errno;
            LxtLogError("Waiting on epoll failed! %d", Result);
            goto cleanup;
        }

        if (Result == 0)
        {
            LxtLogError("Waiting on epoll succeeded but returned zero events!");
            Result = -1;
            goto cleanup;
        }

        LxtLogInfo("[Server] Event: {%d, %x} ", EpollWaitEvent[0].data.fd, EpollWaitEvent[0].events);

        if (EpollWaitEvent[0].data.fd != FileDescriptor2)
        {
            LxtLogError("[Server] Epoll wait satisfied with wrong user data! %d", EpollWaitEvent[0].data.fd);
            Result = -1;
            goto cleanup;
        }

        if (EpollWaitEvent[0].events != EPOLLIN)
        {
            LxtLogError("[Server] Epoll wait satisfied with wrong events! 0x%x", EpollWaitEvent[0].events);
            Result = -1;
            goto cleanup;
        }

        memset(Buffer, 0, sizeof(Buffer));
        Result = read(FileDescriptor2, Buffer, sizeof(Buffer));
        if (Result < 0)
        {
            Result = errno;
            LxtLogError("[Client] Read on socket failed! %d", Result);
            goto cleanup;
        }

        LxtLogInfo("[Server] read %d bytes: %s ...", Result, Buffer);

        if (Result == 0)
        {
            LxtLogInfo("[Server] exiting ...");
            goto cleanup;
        }
    }

    LxtLogInfo("[Server] Waiting on child");

    wait(&ChildStatus);

    LxtLogInfo("[Server] Child WIFEXITED=%d WEXITSTATUS=%d", WIFEXITED(ChildStatus), WEXITSTATUS(ChildStatus));

    //
    // Determine if the test passed or failed.
    //

    if ((Result < 0) || (WIFEXITED(ChildStatus) == 0) || (WEXITSTATUS(ChildStatus) != 0))
    {

        LxtLogInfo("[Server] Test failed!");
        Result = -1;
    }

    LxtLogInfo("[Server] Done");

cleanup:

    if (FileDescriptor1 != -1)
    {
        close(FileDescriptor1);
    }

    if (EpollFileDescriptor != -1)
    {
        close(EpollFileDescriptor);
    }

    if (FileDescriptor2 != -1)
    {
        close(FileDescriptor2);
    }

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int EpollVariation0(PLXT_ARGS Args)

/*++
--*/

{

    char Buffer[10];
    int BytesReadWrite;
    int ChildPid;
    struct epoll_event EpollControlEvent;
    int FileDescriptor1;
    int FileDescriptor2;
    int EpollFileDescriptor;
    struct epoll_event EpollWaitEvent[2];
    int Index;
    int Master;
    char PtsDevName[50];
    int Result;
    int Status;

    //
    // Initialize locals.
    //

    Master = -1;
    FileDescriptor1 = -1;
    FileDescriptor2 = -1;
    EpollFileDescriptor = -1;
    ChildPid = -1;

    //
    // Open a file that will be added to the epoll.
    //

    LxtCheckErrno(Master = open("/dev/ptmx", O_RDWR));
    LxtCheckErrno(grantpt(Master));
    LxtCheckErrno(unlockpt(Master));
    LxtCheckErrno(ptsname_r(Master, PtsDevName, sizeof(PtsDevName)));
    LxtLogInfo("Subordinate Device is:%s", PtsDevName);
    LxtCheckErrno(FileDescriptor1 = open(PtsDevName, O_RDWR));
    LxtCheckErrno(FileDescriptor2 = open(PtsDevName, O_RDWR));

    //
    // Create an epoll.
    //

    LxtCheckErrno(EpollFileDescriptor = epoll_create(1));

    //
    // Add the file to the epoll.
    //

    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = FileDescriptor1;
    LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, FileDescriptor1, &EpollControlEvent));

    //
    // Add the file to the epoll again and it should fail.
    //

    LxtCheckErrnoFailure(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, FileDescriptor1, &EpollControlEvent), EEXIST);

    //
    // Add the epoll file descriptor to itself and it should fail.
    //

    LxtCheckErrnoFailure(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, EpollFileDescriptor, &EpollControlEvent), EINVAL);

    //
    // Add the second file descriptor to the epoll.
    //

    EpollControlEvent.events = EPOLLOUT | EPOLLPRI;
    EpollControlEvent.data.fd = FileDescriptor2;
    LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, FileDescriptor2, &EpollControlEvent));

    //
    // Remove the second file from the epoll.
    //

    LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_DEL, FileDescriptor2, NULL));

    //
    // Try adding back the first file descriptor as it should still be there.
    //

    LxtCheckErrnoFailure(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, FileDescriptor1, &EpollControlEvent), EEXIST);

    //
    // Add the second file descriptor back to the epoll.
    //

    EpollControlEvent.events = EPOLLOUT | EPOLLPRI;
    EpollControlEvent.data.fd = FileDescriptor2;
    LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, FileDescriptor2, &EpollControlEvent));

    //
    // Modify the second file descriptor in the epoll.
    //

    EpollControlEvent.events = EPOLLIN | EPOLLERR | EPOLLPRI | EPOLLET;
    EpollControlEvent.data.fd = FileDescriptor2;
    LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_MOD, FileDescriptor2, &EpollControlEvent));

    //
    // Wait for the epoll with a timeout.
    //

    LxtLogInfo("Waiting on epoll to timeout for 1s...");

    LxtCheckErrnoZeroSuccess(epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, 200));

    //
    // Fork to create another thread to signal the epoll.
    //

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {

        //
        // Wait to allow parent to block on epoll.
        //

        LXT_SYNCHRONIZATION_POINT();
        LxtLogInfo("T2: Waiting to make read data available...");
        usleep(200 * 1000);
        LxtLogInfo("T2: Making data available for read...");
        LxtCheckErrno((BytesReadWrite = write(Master, "\n", 1)));

        LXT_SYNCHRONIZATION_POINT();
        usleep(200 * 1000);
        LxtLogInfo("T2: Making data available for read...");
        LxtCheckErrno(BytesReadWrite = read(FileDescriptor1, Buffer, sizeof(Buffer)));
        LxtCheckEqual(BytesReadWrite, 1, "%d");
        LxtCheckErrno((BytesReadWrite = write(Master, "\n", 1)));

        LxtLogInfo("T2: Waiting to allow T1 to wake, consume edge trigger, and wait again...");
        LXT_SYNCHRONIZATION_POINT();
        usleep(200 * 1000);
        LxtLogInfo("T2: Clearing edge-trigger on descriptor2...");
        LxtCheckErrno(BytesReadWrite = read(FileDescriptor1, Buffer, sizeof(Buffer)));
        LxtCheckEqual(BytesReadWrite, 1, "%d");
        LXT_SYNCHRONIZATION_POINT();

        LXT_SYNCHRONIZATION_POINT();
        LxtLogInfo("T2: Making data available for read...");
        LxtCheckErrno((BytesReadWrite = write(Master, "\n", 1)));

        Result = LXT_RESULT_SUCCESS;
        goto ErrorExit;
    }

    //
    // Wait on epoll to be woken by the child. Do this twice and the second time
    // should immediately return since the first epoll is still signalled.
    //

    for (Index = 0; Index < 2; Index += 1)
    {

        LxtLogInfo("T1: Waiting for epoll to be signaled for first descriptor...");
        LXT_SYNCHRONIZATION_POINT();
        LxtCheckErrno(epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, -1));
        LxtCheckEqual(Result, (2 - Index), "%d");
        if (EpollWaitEvent[0].data.fd == FileDescriptor1)
        {
            LxtCheckEqual(EpollWaitEvent[0].data.fd, FileDescriptor1, "%d");
            LxtCheckEqual(EpollWaitEvent[0].events, EPOLLIN, "%d");
            if (Result > 1)
            {
                LxtCheckEqual(EpollWaitEvent[1].data.fd, FileDescriptor2, "%d");
                LxtCheckEqual(EpollWaitEvent[1].events, EPOLLIN, "%d");
            }
        }
        else
        {
            LxtCheckEqual(EpollWaitEvent[0].data.fd, FileDescriptor2, "%d");
            LxtCheckEqual(EpollWaitEvent[0].events, EPOLLIN, "%d");
            if (Result > 1)
            {
                LxtCheckEqual(EpollWaitEvent[1].data.fd, FileDescriptor1, "%d");
                LxtCheckEqual(EpollWaitEvent[1].events, EPOLLIN, "%d");
            }
        }

        // LxtCheckErrno(BytesReadWrite = read(FileDescriptor1, Buffer, sizeof(Buffer)));
        // LxtCheckEqual(BytesReadWrite, 1, "%d");
        if (Index == 0)
        {

            //
            // Modify the first file descriptor in the epoll to be one shot. This
            // way, when it's waited on again in the next iteration of the loop,
            // it will be disabled.
            //

            EpollControlEvent.events = EPOLLIN | EPOLLONESHOT;
            EpollControlEvent.data.fd = FileDescriptor1;
            LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_MOD, FileDescriptor1, &EpollControlEvent));
        }
    }

    //
    // Now wait for the epoll to be signalled by the child for the second
    // descriptor. That registration was with an edge trigger.
    //

    LXT_SYNCHRONIZATION_POINT();
    LxtLogInfo("T1: Waiting for epoll to be signaled for second descriptor...");
    LxtCheckErrno(epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, -1));
    LxtCheckEqual(Result, 1, "%d");
    LxtCheckEqual(EpollWaitEvent[0].data.fd, FileDescriptor2, "%d");
    LxtCheckEqual(EpollWaitEvent[0].events, EPOLLIN, "%d");

    //
    // Wait for the epoll again and it should timeout this time due to edge
    // trigger.
    //

    LXT_SYNCHRONIZATION_POINT();
    LxtLogInfo("Waiting on epoll to timeout...");
    LxtCheckErrnoZeroSuccess(epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, 200));

    //
    // Signal the event again, but descriptor 1 is marked oneshot so it still
    // won't deliver any notifications.
    //

    LXT_SYNCHRONIZATION_POINT();
    LxtLogInfo("Waiting on epoll (T1 to ready data) indefinitely...");
    LxtCheckErrno(epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, -1));
    LxtCheckEqual(Result, 1, "%d");
    LxtCheckEqual(EpollWaitEvent[0].data.fd, FileDescriptor2, "%d");
    LxtCheckEqual(EpollWaitEvent[0].events, EPOLLIN, "%d");

    //
    // Making data unavailable for both descriptors. And generating error on
    // second descriptor.
    //

    LxtCheckClose(Master);
    LxtLogInfo("Waiting on epoll for error indefinitely...");
    LxtCheckErrno(epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, -1));
    LxtCheckEqual(Result, 1, "%d");
    LxtCheckEqual(EpollWaitEvent[0].data.fd, FileDescriptor2, "%d");

    //
    // TODO_LX: Currently only signalling EPOLLHUP.
    //
    // LxtCheckEqual(EpollWaitEvent[0].events, (EPOLLHUP | EPOLLERR | EPOLLIN), "%d");
    //

    Result = LXT_RESULT_SUCCESS;

ErrorExit:

    //
    // Close the file descriptors in a specific order to exercise both code
    // paths where a file is closed while in an epoll and epoll is closed while
    // having files in it.
    //

    if (FileDescriptor2 != -1)
    {
        close(FileDescriptor2);
    }

    if (EpollFileDescriptor != -1)
    {
        close(EpollFileDescriptor);
    }

    if (FileDescriptor1 != -1)
    {
        close(FileDescriptor1);
    }

    if (Master != -1)
    {
        close(Master);
    }

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

typedef struct _READD_TEST_DATA
{
    int Fd;
    int EpollFd;
} READD_TEST_DATA, *PREADD_TEST_DATA;

static READD_TEST_DATA ReAddTestData;

static int EpollReAddTestClone(void* Parameter)

/*++
--*/

{

    struct epoll_event EpollControlEvent;
    int Result = -1;

    //
    // Try to close file descriptor already added to epoll.
    //

    EpollControlEvent.events = EPOLLIN | EPOLLET;
    EpollControlEvent.data.fd = ReAddTestData.Fd;
    LxtLogInfo("[Cloned] Trying to add existing fd (%d) to epoll context", ReAddTestData.Fd);

    LxtCheckErrnoFailure(epoll_ctl(ReAddTestData.EpollFd, EPOLL_CTL_ADD, ReAddTestData.Fd, &EpollControlEvent), EEXIST);

    LxtLogInfo("[Cloned] Closing fd (%d) in shared file descriptor table", ReAddTestData.Fd);

    LxtClose(ReAddTestData.Fd);
    Result = 0;

ErrorExit:
    LxtLogInfo("[Cloned] Exiting...");
    exit(Result);
}

int EpollAddTest(PLXT_ARGS Args)

/*++
--*/

{
    int Result;
    int ChildPid;
    struct epoll_event EpollControlEvent;
    int EpollFd;
    int SocketFd1;
    int SocketFdCopy;
    int SocketFd2;
    int SocketFd3;
    int SocketFd4;
    int Status;
    LXT_CLONE_ARGS CloneArgs;

    SocketFd1 = -1;
    SocketFd2 = -1;
    SocketFd3 = -1;
    EpollFd = -1;
    int Flags;

    LxtCheckErrno(EpollFd = epoll_create1(EPOLL_CLOEXEC));
    LxtCheckErrno(SocketFd1 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    LxtCheckErrno(SocketFd3 = dup(SocketFd1));
    LxtLogInfo("Created fd1 (%d), duplicated into fd3 (%d)", SocketFd1, SocketFd3);
    EpollControlEvent.events = EPOLLIN | EPOLLET;
    EpollControlEvent.data.fd = SocketFd1;
    LxtLogInfo("Adding fd1 (%d) file descriptor to epoll context", SocketFd1);
    LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd1, &EpollControlEvent));

    LxtLogInfo("Adding fd3 (%d) file descriptor to epoll context", SocketFd3);
    EpollControlEvent.data.fd = SocketFd3;
    LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd3, &EpollControlEvent));

    LxtLogInfo("Closing fd1 (%d) file descriptor", SocketFd1);
    LxtClose(SocketFd1);
    SocketFdCopy = SocketFd1;
    SocketFd1 = -1;
    LxtCheckErrno(SocketFd2 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    LxtLogInfo("Created fd2 (%d) file descriptor, that should be the same as closed fd1 (%d)", SocketFd2, SocketFdCopy);

    LxtCheckEqual(SocketFdCopy, SocketFd2, "%d");
    LxtCheckNotEqual(SocketFd1, SocketFd3, "%d");
    EpollControlEvent.data.fd = SocketFd2;
    LxtLogInfo("Adding fd2 (%d) file descriptor to epoll context", SocketFd2);
    LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd2, &EpollControlEvent));

    LxtLogInfo("Trying to add fd3 (%d) file descriptor, expecting EEXIST", SocketFd3);
    EpollControlEvent.data.fd = SocketFd3;
    LxtCheckErrnoFailure(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd3, &EpollControlEvent), EEXIST);

    LxtLogInfo("Forking...");
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LXT_SYNCHRONIZATION_POINT();

        LxtLogInfo("[Forked] Try to add fd3 (%d) already added to epoll context", SocketFd3);
        LxtCheckErrnoFailure(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd3, &EpollControlEvent), EEXIST);

        LxtLogInfo("[Forked] Closing fd3 (%d), should not affect parent", SocketFd3);
        LxtClose(SocketFd3);
        SocketFd3 = -1;
        LXT_SYNCHRONIZATION_POINT();

        //
        // Let the parent try to add already existing file descriptor, verifying
        // that it hasn't been removed in both file descriptor tables.
        //

        LXT_SYNCHRONIZATION_POINT();

        LxtCheckErrno(SocketFd3 = dup(SocketFd2));
        LxtLogInfo("[Forked] Duplicated fd2 (%d) to fd3 (%d) and adding fd3 to epoll context", SocketFd2, SocketFd3);
        EpollControlEvent.data.fd = SocketFd3;
        LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd3, &EpollControlEvent));

        goto ErrorExit;
    }

    LXT_SYNCHRONIZATION_POINT();

    //
    // Let the child process close the file descriptor in the cloned file
    // descriptor table.
    //

    LXT_SYNCHRONIZATION_POINT();

    LxtLogInfo("Try to add fd3 (%d) already added to epoll context", SocketFd3);
    EpollControlEvent.data.fd = SocketFd3;
    LxtCheckErrnoFailure(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd3, &EpollControlEvent), EEXIST);

    LXT_SYNCHRONIZATION_POINT();

ErrorExit:
    if (SocketFd1 != -1)
    {
        close(SocketFd1);
    }

    if (SocketFd2 != -1)
    {
        close(SocketFd2);
    }

    if (SocketFd3 != -1)
    {
        close(SocketFd3);
    }

    if (EpollFd != -1)
    {
        close(EpollFd);
    }

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int EpollDeleteTest(PLXT_ARGS Args)

/*++
--*/

{
    int Result;
    int ChildPid;
    struct epoll_event EpollControlEvent;
    int EpollFd;
    int SocketFd;
    int SocketFdCopy;
    int SocketFdDup;
    int Status;
    LXT_CLONE_ARGS CloneArgs;

    SocketFd = -1;
    EpollFd = -1;
    int Flags;

    LxtCheckErrno(EpollFd = epoll_create1(EPOLL_CLOEXEC));
    LxtCheckErrno(SocketFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    LxtLogInfo("Created socket file descriptor (%d)", SocketFd);
    EpollControlEvent.events = EPOLLIN | EPOLLET;
    EpollControlEvent.data.fd = SocketFd;
    LxtLogInfo("Adding socket file descriptor (%d) to epoll context", SocketFd);
    LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd, &EpollControlEvent));

    LxtLogInfo("1. Forking...");
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtLogInfo("[Forked] Try to add socket file descriptor (%d) already added to epoll context", SocketFd);
        LxtCheckErrnoFailure(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd, &EpollControlEvent), EEXIST);

        LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_DEL, SocketFd, NULL));

        LXT_SYNCHRONIZATION_POINT();

        //
        // Let the parent re-add socket file descriptor.
        //

        LXT_SYNCHRONIZATION_POINT();
        goto ErrorExit;
    }

    //
    // Let the child process remove the file descriptor in the cloned file
    // descriptor table.
    //

    LXT_SYNCHRONIZATION_POINT();
    LxtLogInfo("Try to add socket file descriptor (%d)", SocketFd);
    LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd, &EpollControlEvent));

    LXT_SYNCHRONIZATION_POINT();
    LXT_SYNCHRONIZATION_POINT_END();
    ChildPid = -1;
    LxtCheckResult(Result);

    LxtLogInfo("2. Forking...");
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtLogInfo("[Forked] Closing original socket fd and creating a new socket fd, should be equal");
        SocketFdCopy = SocketFd;
        LxtClose(SocketFd);
        LxtCheckErrno(SocketFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        LxtCheckEqual(SocketFd, SocketFdCopy, "%d");
        LxtLogInfo("[Forked] Trying to delete socket file descriptor (%d), should fail", SocketFd);
        LxtCheckErrnoFailure(epoll_ctl(EpollFd, EPOLL_CTL_DEL, SocketFd, NULL), ENOENT);

        LxtLogInfo("[Forked] Try to add socket file descriptor (%d)", SocketFd);
        EpollControlEvent.data.fd = SocketFd;
        LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd, &EpollControlEvent));

        LXT_SYNCHRONIZATION_POINT();

        //
        // Let parent run.
        //

        LXT_SYNCHRONIZATION_POINT();

        LxtLogInfo("[Forked] Trying to delete socket file descriptor (%d) again", SocketFd);
        LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_DEL, SocketFd, NULL));

        LXT_SYNCHRONIZATION_POINT();

        //
        // Let the parent re-add socket file descriptor.
        //

        LXT_SYNCHRONIZATION_POINT();
        goto ErrorExit;
    }

    //
    // Let the child process remove the file descriptor in the cloned file
    // descriptor table.
    //

    LXT_SYNCHRONIZATION_POINT();

    LxtLogInfo("Try to add socket file descriptor (%d), should fail", SocketFd);
    LxtCheckErrnoFailure(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd, &EpollControlEvent), EEXIST);

    LXT_SYNCHRONIZATION_POINT();

    //
    // Let child run.
    //

    LXT_SYNCHRONIZATION_POINT();

    LxtLogInfo("Try to add socket file descriptor (%d), should fail again", SocketFd);
    LxtCheckErrnoFailure(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd, &EpollControlEvent), EEXIST);

    LXT_SYNCHRONIZATION_POINT();
    LXT_SYNCHRONIZATION_POINT_END();
    ChildPid = -1;
    LxtCheckResult(Result);

    LxtLogInfo("3.Forking...");
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtLogInfo("[Forked] Duplicating original socket fd and creating a new socket fd, should be equal");
        LxtCheckErrno(SocketFdDup = dup(SocketFd));
        LxtLogInfo("[Forked] Closing original socket fd and creating a new socket fd, should be equal");
        SocketFdCopy = SocketFd;
        LxtClose(SocketFd);
        LxtLogInfo("[Forked] Duplicating original socket fd into new socket fd, should be equal as the closed socket fd");
        LxtCheckErrno(SocketFd = dup(SocketFdDup));
        LxtCheckEqual(SocketFd, SocketFdCopy, "%d");
        LxtLogInfo("[Forked] Trying to delete socket file descriptor (%d)", SocketFd);
        LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_DEL, SocketFd, NULL));

        LXT_SYNCHRONIZATION_POINT();

        //
        // Let the parent run.
        //

        LXT_SYNCHRONIZATION_POINT();
        goto ErrorExit;
    }

    //
    // Let the child process remove the file descriptor in the cloned file
    // descriptor table.
    //

    LXT_SYNCHRONIZATION_POINT();

    LxtLogInfo("Try to add socket file descriptor (%d)", SocketFd);
    LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketFd, &EpollControlEvent));

    LXT_SYNCHRONIZATION_POINT();

    //
    // Let the child process finish.
    //

ErrorExit:
    if (SocketFd != -1)
    {
        close(SocketFd);
    }

    if (EpollFd != -1)
    {
        close(EpollFd);
    }

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int EpollModifyWhilePollingTest(PLXT_ARGS Args)

/*++
--*/

{

    int ChildPid;
    struct epoll_event EpollControlEvent;
    int EpollFileDescriptor;
    struct epoll_event EpollWaitEvent[2];
    struct epoll_event* InputEvent;
    struct epoll_event* OutputEvent;
    int PipeFileDescriptors[2] = {-1, -1};
    int Result;
    int Status;

    //
    // Initialize locals.
    //

    ChildPid = -1;
    EpollFileDescriptor = -1;

    LxtCheckResult(LxtSignalInitialize());
    LxtCheckResult(LxtSignalSetupHandler(SIGPIPE, SA_SIGINFO));

    //
    // Open a pipe to test epoll.
    //

    LxtCheckErrnoZeroSuccess(pipe(PipeFileDescriptors));

    //
    // Create an epoll.
    //

    LxtCheckErrno(EpollFileDescriptor = epoll_create(1));

    //
    // Add the file to the epoll.
    //

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckClose(PipeFileDescriptors[0]);
        LxtCheckClose(PipeFileDescriptors[1]);
        LXT_SYNCHRONIZATION_POINT();
        LxtLogInfo("Waiting on epoll...", Result);
        LxtCheckErrno(epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, 2000));
        LxtCheckEqual(Result, 1, "%d");
        LxtCheckEqual(EpollWaitEvent[0].events, EPOLLOUT, "%d");
        LxtCheckEqual(EpollWaitEvent[0].data.fd, 0, "%d");
        LXT_SYNCHRONIZATION_POINT();
        LXT_SYNCHRONIZATION_POINT();
        LxtLogInfo("Waiting on epoll...", Result);
        LxtCheckErrnoZeroSuccess(epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, 2000));
        goto ErrorExit;
    }

    LXT_SYNCHRONIZATION_POINT();
    sleep(1);
    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = 0;
    LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, 0, &EpollControlEvent));

    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = PipeFileDescriptors[0];
    LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, PipeFileDescriptors[0], &EpollControlEvent));

    EpollControlEvent.events = EPOLLOUT | EPOLLONESHOT;
    EpollControlEvent.data.fd = 0;
    LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_MOD, 0, &EpollControlEvent));

    LXT_SYNCHRONIZATION_POINT();

    //
    // The child consumed the epoll event so it should no longer be available.
    //

    LxtCheckErrno(epoll_wait(EpollFileDescriptor, EpollWaitEvent, 2, 0));
    LxtCheckEqual(Result, 0, "%d");

    LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_DEL, 0, NULL));

    LXT_SYNCHRONIZATION_POINT();
    sleep(1);
    LxtLogInfo("Closing last file descriptor in epoll.", Result);
    LxtCheckClose(PipeFileDescriptors[0]);

    //
    // The epoll is active, but the pipe it was waiting on is now closed.
    //

    LxtCheckErrnoFailure(write(PipeFileDescriptors[1], "\n", 1), EPIPE);
    LxtCheckResult(LxtSignalCheckReceived(SIGPIPE));
    LxtSignalResetReceived();
    LxtCheckClose(PipeFileDescriptors[1]);

ErrorExit:
    if (EpollFileDescriptor != -1)
    {
        close(EpollFileDescriptor);
    }

    if (PipeFileDescriptors[1] != -1)
    {
        close(PipeFileDescriptors[1]);
    }

    if (PipeFileDescriptors[0] != -1)
    {
        close(PipeFileDescriptors[0]);
    }

    LxtLogInfo("Done, PID=%d, ChildPid=%d", getpid(), ChildPid);
    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int EpollModTest(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    int ChildPid;
    struct epoll_event EpollControlEvent;
    struct epoll_event EpollWaitEvent[2];
    int SocketFd;
    int TmpFd;
    int EpollFd;
    LXT_SOCKET_PAIR SocketPair;
    int Status;

    SocketPair.Parent = -1;
    SocketPair.Child = -1;

    LxtCheckErrno(EpollFd = epoll_create1(EPOLL_CLOEXEC));
    LxtCheckResult(LxtSocketPairCreate(&SocketPair));
    LxtLogInfo("Created socket pair (%d, %d)", SocketPair.Parent, SocketPair.Child);
    EpollControlEvent.events = EPOLLET;
    EpollControlEvent.data.fd = SocketPair.Parent;
    LxtLogInfo("Adding socket pair (parent) (%d) file descriptor to epoll context", SocketPair.Parent);

    LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketPair.Parent, &EpollControlEvent));

    LxtLogInfo("Adding socket pair (child) (%d) file descriptor to epoll context", SocketPair.Child);

    EpollControlEvent.data.fd = SocketPair.Child;
    LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketPair.Child, &EpollControlEvent));

    LxtLogInfo("Forking...");
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LXT_SYNCHRONIZATION_POINT();

        LxtLogInfo("[Child] Waiting on epoll (EPOLLIN), should timeout");
        LxtCheckErrnoZeroSuccess(epoll_wait(EpollFd, EpollWaitEvent, 2, 1000));

        LXT_SYNCHRONIZATION_POINT();

        LxtLogInfo("[Child] Waiting on epoll (EPOLLIN)");
        LxtCheckErrno(Result = epoll_wait(EpollFd, EpollWaitEvent, 2, -1));
        LxtLogInfo("[Child] EPOLLIN received");
        LxtCheckEqual(Result, 1, "%d");
        LxtCheckEqual(EpollWaitEvent[0].data.fd, SocketPair.Child, "%d");
        LxtCheckEqual(EpollWaitEvent[0].events, EPOLLIN, "0x%x");
        LxtCheckResult(LxtReceiveMessage(SocketPair.Child, "data"));
        LxtLogInfo("[Child] Received message");

        LXT_SYNCHRONIZATION_POINT();

        //
        // Wait for parent to send data.
        //

        LXT_SYNCHRONIZATION_POINT();
        goto ErrorExit;
    }

    LxtLogInfo("Sending data over socketpair");
    LxtCheckResult(LxtSendMessage(SocketPair.Parent, "data"));

    LXT_SYNCHRONIZATION_POINT();

    //
    // Let the child wait/timeout.
    //

    LXT_SYNCHRONIZATION_POINT();

    LxtLogInfo("Modifying the epoll to receive EPOLLIN events");
    EpollControlEvent.events = EPOLLIN | EPOLLET;
    EpollControlEvent.data.fd = SocketPair.Child;
    LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_MOD, SocketPair.Child, &EpollControlEvent));

    LXT_SYNCHRONIZATION_POINT();

    //
    // Wait for child to receive data.
    //

    LXT_SYNCHRONIZATION_POINT();

    Result = 0;

ErrorExit:
    if (SocketPair.Parent != -1)
    {
        close(SocketPair.Parent);
    }

    if (SocketPair.Child != -1)
    {
        close(SocketPair.Child);
    }

    if (EpollFd != -1)
    {
        close(EpollFd);
    }

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int EpollPhantomEventsTest(PLXT_ARGS Args)

/*++
--*/

{

    int Result;
    int ChildPid;
    struct epoll_event EpollControlEvent;
    struct epoll_event EpollWaitEvent[2];
    int SocketFd;
    int TmpFd;
    int EpollFd;
    LXT_SOCKET_PAIR SocketPair;
    int Status;

    SocketPair.Parent = -1;
    SocketPair.Child = -1;

    LxtCheckErrno(EpollFd = epoll_create1(EPOLL_CLOEXEC));
    LxtCheckResult(LxtSocketPairCreate(&SocketPair));
    LxtLogInfo("Created socket pair (%d, %d)", SocketPair.Parent, SocketPair.Child);
    EpollControlEvent.events = EPOLLIN | EPOLLET;
    EpollControlEvent.data.fd = SocketPair.Parent;
    LxtLogInfo("Adding socket pair (parent) (%d) file descriptor to epoll context", SocketPair.Parent);

    LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketPair.Parent, &EpollControlEvent));

    LxtLogInfo("Adding socket pair (child) (%d) file descriptor to epoll context", SocketPair.Child);

    EpollControlEvent.data.fd = SocketPair.Child;
    LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, SocketPair.Child, &EpollControlEvent));

    LxtLogInfo("Forking...");
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtLogInfo("[Child] Waiting on epoll (EPOLLIN)");
        LxtCheckErrno(Result = epoll_wait(EpollFd, EpollWaitEvent, 2, -1));
        LxtCheckEqual(Result, 1, "%d");
        LxtCheckEqual(EpollWaitEvent[0].data.fd, SocketPair.Child, "%d");
        LxtCheckEqual(EpollWaitEvent[0].events, EPOLLIN, "0x%x");
        LxtLogInfo("[Child] Waiting on message");
        LxtCheckResult(LxtReceiveMessage(SocketPair.Child, "data"));

        LxtLogInfo("Closing (%d) file descriptor", SocketPair.Child);
        TmpFd = SocketPair.Child;
        LxtClose(SocketPair.Child);

        LxtCheckErrno(SocketFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        LxtLogInfo("Created (%d) file descriptor, that should be the same as closed fd (%d)", SocketFd, TmpFd);

        LXT_SYNCHRONIZATION_POINT();

        //
        // Wait for parent to send data.
        //

        LXT_SYNCHRONIZATION_POINT();

        LxtLogInfo("Waiting on epoll phantom event (EPOLLIN)");
        LxtCheckErrno(Result = epoll_wait(EpollFd, EpollWaitEvent, 2, -1));
        LxtCheckEqual(Result, 1, "%d");
        LxtCheckEqual(EpollWaitEvent[0].data.fd, SocketPair.Child, "%d");
        LxtCheckEqual(EpollWaitEvent[0].events, EPOLLIN, "0x%x");

        LXT_SYNCHRONIZATION_POINT();
        goto ErrorExit;
    }

    LxtLogInfo("Sending data over socketpair");
    LxtCheckResult(LxtSendMessage(SocketPair.Parent, "data"));

    //
    // Verify that both only child receives the same EPOLLIN event.
    //

    sleep(1);
    LxtLogInfo("Waiting on epoll (EPOLLIN) (should fail)");
    LxtCheckErrno(Result = epoll_wait(EpollFd, EpollWaitEvent, 2, 100));
    LxtCheckEqual(Result, 0, "%d");

    LXT_SYNCHRONIZATION_POINT();

    LxtCheckResult(LxtSendMessage(SocketPair.Parent, "data"));

    LXT_SYNCHRONIZATION_POINT();

    //
    // Wait for child to receive phantom event.
    //

    LXT_SYNCHRONIZATION_POINT();

    Result = 0;

ErrorExit:
    if (SocketPair.Parent != -1)
    {
        close(SocketPair.Parent);
    }

    if (SocketPair.Child != -1)
    {
        close(SocketPair.Child);
    }

    if (EpollFd != -1)
    {
        close(EpollFd);
    }

    LXT_SYNCHRONIZATION_POINT_END();
    return Result;
}

int PPollInvalidArgument(PLXT_ARGS Args)

{

    int FileDescriptor1;
    struct pollfd PollDescriptors[4];
    int Result;
    struct timespec Timeout;

    LxtCheckResult(FileDescriptor1 = open("/data/test/poll_test.bin", O_RDWR | O_CREAT, S_IRWXU));
    PollDescriptors[0].fd = FileDescriptor1;
    PollDescriptors[0].events = POLLIN;
    PollDescriptors[0].revents = -1;

    //
    // Invalid argument variations.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    Timeout.tv_sec = -1;
    LxtCheckErrnoFailure(ppoll(PollDescriptors, 1, &Timeout, NULL), EINVAL);
    memset(&Timeout, 0, sizeof(Timeout));
    Timeout.tv_nsec = 999999999 + 1;
    LxtCheckErrnoFailure(ppoll(PollDescriptors, 1, &Timeout, NULL), EINVAL);

ErrorExit:
    if (FileDescriptor1 != -1)
    {
        LxtClose(FileDescriptor1);
    }

    return Result;
}

int EpollUnalignedTest(PLXT_ARGS Args)

{

    char Buffer[sizeof(struct pollfd) + 1];
    int FileDescriptor1;
    struct pollfd* PollDescriptors;
    int Result;
    struct timespec Timeout;

    //
    // Set up the poll descriptors array to be unaligned.
    //

    PollDescriptors = (struct pollfd*)&Buffer[1];
    LxtLogInfo("PollDescriptors address %p", PollDescriptors);

    LxtCheckResult(FileDescriptor1 = open("/data/test/poll_test.bin", O_RDWR | O_CREAT, S_IRWXU));
    PollDescriptors[0].fd = FileDescriptor1;
    PollDescriptors[0].events = POLLIN;
    PollDescriptors[0].revents = -1;
    memset(&Timeout, 0, sizeof(Timeout));
    Timeout.tv_sec = 1;
    LxtCheckErrno(ppoll(PollDescriptors, 1, &Timeout, NULL));

ErrorExit:
    if (FileDescriptor1 != -1)
    {
        LxtClose(FileDescriptor1);
    }

    return Result;
}

int EpollDeleteCloseFdLoop(PLXT_ARGS Args)

/*++

Routine Description:

    This routine loops\stresses the removal of EPOLL FD and closing of the FD.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[10];
    int ChildPid;
    struct epoll_event EpollControlEvent;
    const int NumFd = 100;
    int EpollFd[NumFd];
    int Iterator;
    int Loop;
    int NestedEpollFd[NumFd];
    int PipeFileDescriptors[2] = {-1, -1};
    int Result;
    int SharedEpollFd;
    int SocketFd[NumFd];
    int Status;

    //
    // Initialize locals.
    //

    ChildPid = -1;
    for (Iterator = 0; Iterator < NumFd; Iterator++)
    {
        SocketFd[Iterator] = -1;
        EpollFd[Iterator] = -1;
        NestedEpollFd[Iterator] = -1;
    }

    LxtCheckErrnoZeroSuccess(pipe(PipeFileDescriptors));
    LxtCheckErrno(write(PipeFileDescriptors[1], "\n", 1));
    LxtCheckErrno(SharedEpollFd = epoll_create1(0));

    //
    // Start a loop to read the pipe, thereby triggering a notification.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckClose(PipeFileDescriptors[1]);
        while ((Result = read(PipeFileDescriptors[0], Buffer, sizeof(Buffer))) > 0)
            ;

        //
        // The loop should terminate when the other threads exit and
        // close the write pipe handle.
        //

        LxtCheckErrno(Result);
        goto ErrorExit;
    }

    for (Loop = 0; Loop < 50; Loop++)
    {
        for (Iterator = 0; Iterator < NumFd; Iterator++)
        {
            LxtCheckErrno(EpollFd[Iterator] = epoll_create1(0));
            LxtCheckErrno(NestedEpollFd[Iterator] = epoll_create1(0));
            LxtCheckErrno(SocketFd[Iterator] = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));

            EpollControlEvent.events = EPOLLIN | EPOLLET;
            EpollControlEvent.data.fd = SocketFd[Iterator];
            LxtCheckErrno(epoll_ctl(EpollFd[Iterator], EPOLL_CTL_ADD, SocketFd[Iterator], &EpollControlEvent));

            EpollControlEvent.events = EPOLLIN;
            EpollControlEvent.data.fd = PipeFileDescriptors[0];
            LxtCheckErrno(epoll_ctl(NestedEpollFd[Iterator], EPOLL_CTL_ADD, PipeFileDescriptors[0], &EpollControlEvent));

            EpollControlEvent.events = EPOLLIN;
            EpollControlEvent.data.fd = NestedEpollFd[Iterator];
            LxtCheckErrno(epoll_ctl(EpollFd[Iterator], EPOLL_CTL_ADD, NestedEpollFd[Iterator], &EpollControlEvent));

            EpollControlEvent.events = EPOLLIN;
            EpollControlEvent.data.fd = EpollFd[Iterator];
            LxtCheckErrno(epoll_ctl(SharedEpollFd, EPOLL_CTL_ADD, EpollFd[Iterator], &EpollControlEvent));
        }

        LXT_SYNCHRONIZATION_POINT_START();
        LxtCheckErrno(ChildPid = fork());
        if (ChildPid == 0)
        {

            for (Iterator = 0; Iterator < NumFd; Iterator++)
            {
                LxtClose(NestedEpollFd[Iterator]);
                NestedEpollFd[Iterator] = -1;
                LxtClose(SocketFd[Iterator]);
                SocketFd[Iterator] = -1;
            }

            //
            // The race is between releasing the last reference to the file
            // descriptor here and releasing the last reference to EPOLL by
            // the child. Synchronize to keep the race as close as possible.
            //

            LXT_SYNCHRONIZATION_POINT();
            for (Iterator = 0; Iterator < NumFd; Iterator++)
            {
                LxtClose(EpollFd[Iterator]);
                EpollFd[Iterator] = -1;
            }

            _exit(0);
        }

        for (Iterator = 0; Iterator < NumFd; Iterator++)
        {
            LxtClose(EpollFd[Iterator]);
            EpollFd[Iterator] = -1;
        }

        //
        // See the above comment about synchronizing to keep the race close.
        //

        LXT_SYNCHRONIZATION_POINT();
        for (Iterator = 0; Iterator < NumFd; Iterator++)
        {
            LxtCheckErrno(write(PipeFileDescriptors[1], "\n", 1));
            LxtClose(NestedEpollFd[Iterator]);
            NestedEpollFd[Iterator] = -1;
            LxtClose(SocketFd[Iterator]);
            SocketFd[Iterator] = -1;
        }

        LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (PipeFileDescriptors[1] != -1)
    {
        close(PipeFileDescriptors[1]);
    }

    if (PipeFileDescriptors[0] != -1)
    {
        close(PipeFileDescriptors[0]);
    }

    if (SharedEpollFd != -1)
    {
        close(SharedEpollFd);
    }

    for (Iterator = 0; Iterator < NumFd; Iterator++)
    {

        //
        // Only close socket FD's in the parent because socket FD's are created
        // with CLOSE_ON_EXEC and so are only valid in the parent.
        //

        if ((SocketFd[Iterator] != -1) && (ChildPid != 0))
        {
            close(SocketFd[Iterator]);
        }

        if ((NestedEpollFd[Iterator] != -1) && (ChildPid != 0))
        {
            close(NestedEpollFd[Iterator]);
        }

        if (EpollFd[Iterator] != -1)
        {
            close(EpollFd[Iterator]);
        }
    }

    if (ChildPid == 0)
    {
        _exit(0);
    }

    return Result;
}

void* EpollDup2FdLoopThread(void* Parameter)

/*++

Description:

    This routine is the thread handler for the EpollDup2FdLoop test.

Arguments:

    Parameter - Supplies the thread parameter.

Return Value:

    Returns the thread id on success, -1 on failure.

--*/

{

    PEPOLL_DUP2_CONTEXT Context;
    struct epoll_event EpollControlEvent;
    int Iterator;
    int Loop;
    int Result;

    Context = (PEPOLL_DUP2_CONTEXT)Parameter;
    for (Loop = 0; Loop < 50; Loop++)
    {
        for (Iterator = 0; Iterator < EPOLL_DUP2_FD_COUNT; Iterator++)
        {
            LXT_SYNCHRONIZATION_POINT_CHILD();
            EpollControlEvent.events = EPOLLIN;
            EpollControlEvent.data.fd = Context->Fd[Iterator];
            if (epoll_ctl(Context->EpollFd, EPOLL_CTL_DEL, Context->Fd[Iterator], &EpollControlEvent) != 0)
            {

                Result = errno;

                //
                // Race with parent is expected to cause the fd to be invalid
                // at times (not the same file that was added).
                //

                if (Result == EINVAL)
                {
                    Result = LXT_RESULT_SUCCESS;
                }

                LxtCheckResult(Result);
            }
        }
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_PTHREAD_END_THREAD();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"

    return (void*)Result;

#pragma GCC diagnostic pop
}

int EpollDup2FdLoop(PLXT_ARGS Args)

/*++

Routine Description:

    This routine loops\stresses epoll operations on fd's that are being closed
    by other threads. The close is done via dup2 because dup2 also holds the
    filetable lock, increasing the chances of hitting locking issues.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct epoll_event EpollControlEvent;
    EPOLL_DUP2_CONTEXT Context;
    int Iterator;
    int Loop;
    int NullFd;
    const int NumFd = EPOLL_DUP2_FD_COUNT;
    int Result;
    void* Status;
    pthread_t Thread = 0;

    //
    // Initialize locals.
    //

    Context.EpollFd = -1;
    for (Iterator = 0; Iterator < NumFd; Iterator++)
    {
        Context.Fd[Iterator] = -1;
    }

    LxtCheckErrno(NullFd = open("/dev/null", O_RDWR));
    LxtCheckErrno(Context.EpollFd = epoll_create1(0));
    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckResultError(pthread_create(&Thread, NULL, EpollDup2FdLoopThread, &Context));

    for (Loop = 0; Loop < 50; Loop++)
    {
        for (Iterator = 0; Iterator < NumFd; Iterator++)
        {
            LxtCheckErrno(Context.Fd[Iterator] = open("/dev/null", O_RDWR));
            EpollControlEvent.events = EPOLLIN | EPOLLET;
            EpollControlEvent.data.fd = Context.Fd[Iterator];
            LxtCheckErrno(epoll_ctl(Context.EpollFd, EPOLL_CTL_ADD, Context.Fd[Iterator], &EpollControlEvent));
        }

        for (Iterator = 0; Iterator < NumFd; Iterator++)
        {
            LXT_SYNCHRONIZATION_POINT_PARENT();
            LxtCheckErrno(dup2(NullFd, Context.Fd[Iterator]));
        }

        for (Iterator = 0; Iterator < NumFd; Iterator++)
        {
            LxtClose(Context.Fd[Iterator]);
            Context.Fd[Iterator] = -1;
        }
    }

    pthread_join(Thread, (void**)&Result);
    Thread = 0;
    LxtCheckResult(Result);

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_PTHREAD_END_PARENT(Thread);
    for (Iterator = 0; Iterator < NumFd; Iterator++)
    {
        if (Context.Fd[Iterator] != -1)
        {
            close(Context.Fd[Iterator]);
        }
    }

    if (Context.EpollFd != -1)
    {
        close(Context.EpollFd);
    }

    if (NullFd != -1)
    {
        close(NullFd);
    }

    return Result;
}

int EpollRelatedFileStress(PLXT_ARGS Args)

/*++

Routine Description:

    This routine loops\stresses the usage of multiple epoll files containing
    the same set of fds.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int AddRemoveLoop;
    char Buffer[10];
    int ChildPid;
    struct epoll_event EpollControlEvent[3];
    int EpollFd[2];
    int Iterator;
    int Loop;
    int MasterEpollFd;
    int Result;
    int SharedEpollFd;
    int Status;

    //
    // Initialize locals.
    //

    ChildPid = -1;
    MasterEpollFd = -1;
    for (Iterator = 0; Iterator < LXT_COUNT_OF(EpollFd); Iterator++)
    {
        EpollFd[Iterator] = -1;
    }

    LxtCheckErrno(MasterEpollFd = epoll_create1(0));
    for (Iterator = 0; Iterator < LXT_COUNT_OF(EpollFd); Iterator++)
    {
        LxtCheckErrno(EpollFd[Iterator] = epoll_create1(0));
        EpollControlEvent[0].events = EPOLLIN;
        EpollControlEvent[0].data.fd = EpollFd[Iterator];
        LxtCheckErrno(epoll_ctl(MasterEpollFd, EPOLL_CTL_ADD, EpollFd[Iterator], EpollControlEvent));
    }

    LXT_SYNCHRONIZATION_POINT_START();
    LxtCheckErrno(ChildPid = fork());
    for (Loop = 0; Loop < 2000; Loop++)
    {
        if (ChildPid == 0)
        {

            //
            // Synchronize with the wait loop to try to increase the
            // chances of hitting a race.
            //

            LXT_SYNCHRONIZATION_POINT();
            for (AddRemoveLoop = 0; AddRemoveLoop < LXT_COUNT_OF(EpollControlEvent); AddRemoveLoop++)
            {

                for (Iterator = 0; Iterator < LXT_COUNT_OF(EpollFd); Iterator++)
                {

                    EpollControlEvent[0].events = EPOLLIN;
                    EpollControlEvent[0].data.fd = 0;
                    LxtCheckErrno(epoll_ctl(EpollFd[Iterator], EPOLL_CTL_ADD, 0, EpollControlEvent));

                    EpollControlEvent[0].events = EPOLLOUT;
                    EpollControlEvent[0].data.fd = 1;
                    LxtCheckErrno(epoll_ctl(EpollFd[Iterator], EPOLL_CTL_ADD, 1, EpollControlEvent));

                    EpollControlEvent[0].events = EPOLLOUT;
                    EpollControlEvent[0].data.fd = 2;
                    LxtCheckErrno(epoll_ctl(EpollFd[Iterator], EPOLL_CTL_ADD, 2, EpollControlEvent));
                }

                for (Iterator = 0; Iterator < LXT_COUNT_OF(EpollFd); Iterator++)
                {

                    LxtCheckErrno(epoll_ctl(EpollFd[Iterator], EPOLL_CTL_DEL, 0, NULL));

                    LxtCheckErrno(epoll_ctl(EpollFd[Iterator], EPOLL_CTL_DEL, 1, NULL));

                    LxtCheckErrno(epoll_ctl(EpollFd[Iterator], EPOLL_CTL_DEL, 2, NULL));
                }
            }
        }
        else
        {
            LXT_SYNCHRONIZATION_POINT();
            (void)epoll_wait(MasterEpollFd, EpollControlEvent, LXT_COUNT_OF(EpollControlEvent), 1);

            for (Iterator = 0; Iterator < LXT_COUNT_OF(EpollFd); Iterator++)
            {
                (void)epoll_wait(EpollFd[Iterator], EpollControlEvent, LXT_COUNT_OF(EpollControlEvent), 1);
            }
        }
    }

ErrorExit:
    LXT_SYNCHRONIZATION_POINT_END();
    for (Iterator = 0; Iterator < LXT_COUNT_OF(EpollFd); Iterator++)
    {
        if (EpollFd[Iterator] != -1)
        {
            close(EpollFd[Iterator]);
        }
    }

    if (MasterEpollFd != -1)
    {
        close(MasterEpollFd);
    }

    return Result;
}

int EpollRecursionTest(PLXT_ARGS Args)

/*++

Routine Description:

    This routine verifies epoll file included in epoll file behavior.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[10];
    struct epoll_event EpollControlEvent;
    int EpollFileDescriptor;
    int EpollContainerFd;
    int EpollContainer2Fd;
    struct epoll_event EpollWaitEvent;
    struct epoll_event* InputEvent;
    struct epoll_event* OutputEvent;
    int PipeFileDescriptors[2] = {-1, -1};
    int PipeFileDescriptors2[2] = {-1, -1};
    fd_set ReadFds;
    int Result;
    struct timeval Timeout;
    fd_set WriteFds;

    //
    // Initialize locals.
    //

    EpollFileDescriptor = -1;
    EpollContainerFd = -1;
    EpollContainer2Fd = -1;

    //
    // Create two epoll files.
    //

    LxtCheckErrno(EpollFileDescriptor = epoll_create(1));
    LxtCheckErrno(EpollContainerFd = epoll_create(1));

    //
    // Open a pipe to test epoll.
    //

    LxtCheckErrnoZeroSuccess(pipe(PipeFileDescriptors));
    LxtCheckErrnoZeroSuccess(pipe(PipeFileDescriptors2));

    //
    // Pend a write.
    //

    LxtCheckErrno(write(PipeFileDescriptors[1], "\n", 1));

    //
    // Add one epoll file to the other.
    //

    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = EpollFileDescriptor;
    LxtCheckErrno(epoll_ctl(EpollContainerFd, EPOLL_CTL_ADD, EpollFileDescriptor, &EpollControlEvent));

    //
    // Now attempt to add them in reverse order to create a simple loop.
    //

    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = EpollContainerFd;
    LxtCheckErrnoFailure(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, EpollContainerFd, &EpollControlEvent), ELOOP);

    //
    // Attempt to modify swapping the container/included descriptors.
    //

    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = EpollContainerFd;
    LxtCheckErrnoFailure(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_MOD, EpollContainerFd, &EpollControlEvent), ENOENT);

    //
    // Attempt to delete swapping the container/included descriptors.
    //

    EpollControlEvent.data.fd = EpollContainerFd;
    LxtCheckErrnoFailure(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_DEL, EpollContainerFd, NULL), ENOENT);

    //
    // Add the read pipe end to the epoll.
    //

    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = PipeFileDescriptors[0];
    LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, PipeFileDescriptors[0], &EpollControlEvent));

    //
    // Verify the epoll is signalled.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(EpollFileDescriptor, &ReadFds);
    LxtCheckErrno(select((EpollFileDescriptor + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Add the second pipe, which is not read ready.
    //

    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = PipeFileDescriptors[0];
    LxtCheckErrno(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, PipeFileDescriptors2[0], &EpollControlEvent));

    //
    // Verify the epoll remains signalled.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(EpollFileDescriptor, &ReadFds);
    LxtCheckErrno(select((EpollFileDescriptor + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Pend a write to the other pipe.
    //

    LxtCheckErrno(write(PipeFileDescriptors2[1], "\n", 1));

    //
    // Verify the epoll remains signalled.
    //

    memset(&Timeout, 0, sizeof(Timeout));
    FD_ZERO(&ReadFds);
    FD_SET(EpollFileDescriptor, &ReadFds);
    LxtCheckErrno(select((EpollFileDescriptor + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Create another epoll containing the first.
    //

    LxtCheckErrno(EpollContainerFd = epoll_create(1));

    //
    // Add the first epoll.
    //

    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = EpollFileDescriptor;
    LxtCheckErrno(epoll_ctl(EpollContainerFd, EPOLL_CTL_ADD, EpollFileDescriptor, &EpollControlEvent));

    //
    // Try to add the second back to the first to create a loop.
    //

    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = EpollContainerFd;
    LxtCheckErrnoFailure(epoll_ctl(EpollFileDescriptor, EPOLL_CTL_ADD, EpollContainerFd, &EpollControlEvent), ELOOP);

    //
    // The first epoll should trigger the second.
    //

    LxtCheckErrno(epoll_wait(EpollContainerFd, &EpollWaitEvent, 1, 0));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Add the second pipe directly to the container epoll.
    //

    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = PipeFileDescriptors2[0];
    LxtCheckErrno(epoll_ctl(EpollContainerFd, EPOLL_CTL_ADD, PipeFileDescriptors2[0], &EpollControlEvent));

    //
    // The epoll should remain signalled.
    //

    LxtCheckErrno(epoll_wait(EpollContainerFd, &EpollWaitEvent, 1, 0));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Create yet another epoll to test non-read signals.
    //

    LxtCheckErrno(EpollContainer2Fd = epoll_create(1));
    EpollControlEvent.events = EPOLLOUT;
    EpollControlEvent.data.fd = EpollFileDescriptor;
    LxtCheckErrno(epoll_ctl(EpollContainer2Fd, EPOLL_CTL_ADD, EpollFileDescriptor, &EpollControlEvent));

    FD_ZERO(&WriteFds);
    FD_SET(EpollContainer2Fd, &WriteFds);
    memset(&Timeout, 0, sizeof(Timeout));
    LxtCheckErrno(select((EpollContainer2Fd + 1), NULL, &WriteFds, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");
    LxtCheckErrno(epoll_wait(EpollContainer2Fd, &EpollWaitEvent, 1, 0));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Clear the signal for the second pipe and verify signal state.
    //

    LxtCheckErrno(read(PipeFileDescriptors2[0], Buffer, sizeof(Buffer)));
    FD_SET(EpollFileDescriptor, &ReadFds);
    memset(&Timeout, 0, sizeof(Timeout));
    LxtCheckErrno(select((EpollFileDescriptor + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");
    LxtCheckErrno(epoll_wait(EpollContainerFd, &EpollWaitEvent, 1, 0));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Clear the signal for the first pipe and verify signal state.
    //

    LxtCheckErrno(read(PipeFileDescriptors[0], Buffer, sizeof(Buffer)));
    FD_SET(EpollFileDescriptor, &ReadFds);
    memset(&Timeout, 0, sizeof(Timeout));
    LxtCheckErrno(select((EpollFileDescriptor + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 0, "%d");
    LxtCheckErrno(epoll_wait(EpollContainerFd, &EpollWaitEvent, 1, 0));
    LxtCheckEqual(Result, 0, "%d");

    //
    // Signal both pipes again.
    //

    LxtCheckErrno(write(PipeFileDescriptors2[1], "\n", 1));
    LxtCheckErrno(write(PipeFileDescriptors2[1], "\n", 1));

    //
    // Verify signal.
    //

    FD_SET(EpollFileDescriptor, &ReadFds);
    memset(&Timeout, 0, sizeof(Timeout));
    LxtCheckErrno(select((EpollFileDescriptor + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");
    LxtCheckErrno(epoll_wait(EpollContainerFd, &EpollWaitEvent, 1, 0));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Remove the second pipe from the container and check signal state again.
    //

    LxtCheckErrno(epoll_ctl(EpollContainerFd, EPOLL_CTL_DEL, PipeFileDescriptors2[0], NULL));

    FD_SET(EpollFileDescriptor, &ReadFds);
    memset(&Timeout, 0, sizeof(Timeout));
    LxtCheckErrno(select((EpollFileDescriptor + 1), &ReadFds, NULL, NULL, &Timeout));
    LxtCheckEqual(Result, 1, "%d");
    LxtCheckErrno(epoll_wait(EpollContainerFd, &EpollWaitEvent, 1, 0));
    LxtCheckEqual(Result, 1, "%d");

    //
    // Close the nested epoll and verify signal state.
    //

    LxtCheckClose(EpollFileDescriptor);
    LxtCheckErrno(epoll_wait(EpollContainerFd, &EpollWaitEvent, 1, 0));
    LxtCheckEqual(Result, 0, "%d");

ErrorExit:
    if (EpollContainer2Fd != -1)
    {
        close(EpollContainer2Fd);
    }

    if (EpollFileDescriptor != -1)
    {
        close(EpollFileDescriptor);
    }

    if (EpollContainerFd != -1)
    {
        close(EpollContainerFd);
    }

    if (PipeFileDescriptors2[1] != -1)
    {
        close(PipeFileDescriptors2[1]);
    }

    if (PipeFileDescriptors2[0] != -1)
    {
        close(PipeFileDescriptors2[0]);
    }

    if (PipeFileDescriptors[1] != -1)
    {
        close(PipeFileDescriptors[1]);
    }

    if (PipeFileDescriptors[0] != -1)
    {
        close(PipeFileDescriptors[0]);
    }

    return Result;
}

int EpollRecursionLimitTest(PLXT_ARGS Args)

/*++

Routine Description:

    This routine checks for an upper limit of recursive epolls.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

#define EPOLL_MAX_RECURSION_COUNT 6
#define EPOLL_CHAIN_COUNT 50

    int ChainIndex;
    struct epoll_event EpollControlEvent;
    int EpollFds[EPOLL_CHAIN_COUNT][EPOLL_MAX_RECURSION_COUNT];
    int EpollContentFds[EPOLL_MAX_RECURSION_COUNT];
    struct epoll_event EpollWaitEvent;
    int ExtraEpollFd;
    int Index;
    int PipeFileDescriptors[2] = {-1, -1};
    int Result;

    //
    // Initialize locals.
    //

    ExtraEpollFd = -1;
    memset(EpollFds, -1, sizeof(EpollFds));
    memset(EpollContentFds, -1, sizeof(EpollContentFds));
    memset(&EpollControlEvent, 0, sizeof(EpollControlEvent));
    memset(&EpollWaitEvent, 0, sizeof(EpollWaitEvent));
    EpollControlEvent.events = EPOLLIN;
    LxtCheckErrnoZeroSuccess(pipe(PipeFileDescriptors));

    for (ChainIndex = 0; ChainIndex < EPOLL_CHAIN_COUNT; ChainIndex += 1)
    {
        for (Index = 0; Index < EPOLL_MAX_RECURSION_COUNT; Index += 1)
        {

            //
            // Create a new epoll.
            //

            LxtCheckErrno(EpollFds[ChainIndex][Index] = epoll_create(1));

            //
            // Add the previous epoll to make a chain.
            //

            if (Index > 0)
            {
                LxtCheckErrno(epoll_ctl(EpollFds[ChainIndex][Index], EPOLL_CTL_ADD, EpollFds[ChainIndex][Index - 1], &EpollControlEvent));
            }
        }
    }

    LxtCheckErrno(ExtraEpollFd = epoll_create(1));

    //
    // Attempt to add an epoll exceeding the maximum depth.
    //

    LxtCheckErrnoFailure(epoll_ctl(ExtraEpollFd, EPOLL_CTL_ADD, EpollFds[0][Index - 1], &EpollControlEvent), ELOOP);

    //
    // Add a pipe file descriptor.
    //

    LxtCheckErrno(epoll_ctl(EpollFds[0][5], EPOLL_CTL_ADD, PipeFileDescriptors[0], &EpollControlEvent));

    //
    // Try creating a chain with a regular file descriptor in it.
    //

    for (Index = 0; Index < (EPOLL_MAX_RECURSION_COUNT - 1); Index += 1)
    {

        //
        // Create a new epoll.
        //

        LxtCheckErrno(EpollContentFds[Index] = epoll_create(1));

        //
        // Add a regular file descriptor.
        //

        LxtCheckErrno(epoll_ctl(EpollContentFds[Index], EPOLL_CTL_ADD, PipeFileDescriptors[0], &EpollControlEvent));

        //
        // Add the previous epoll to make a chain.
        //

        if (Index > 0)
        {
            LxtCheckErrno(epoll_ctl(EpollContentFds[Index], EPOLL_CTL_ADD, EpollContentFds[Index - 1], &EpollControlEvent));
        }
    }

    //
    // A non-epoll file descriptor is not allowed to be nested deeper
    // than EPOLL_MAX_RECURSION_COUNT. There is a pipe file descriptor
    // at [0][0] so this add attempt is expected to fail.
    //

    LxtCheckErrno(EpollContentFds[Index] = epoll_create(1));
    LxtCheckErrnoFailure(epoll_ctl(EpollContentFds[Index], EPOLL_CTL_ADD, EpollContentFds[Index - 1], &EpollControlEvent), EINVAL);

    //
    // Attempt to link chains together, where each add stays below the limit
    // but the total chain size becomes increasingly large.
    //

    for (ChainIndex = (EPOLL_CHAIN_COUNT - 1); ChainIndex > 0; ChainIndex -= 1)
    {
        LxtLogInfo("[%d][%d] -> [%d][%d]", ChainIndex, 0, ChainIndex - 1, Index - 2);
        LxtCheckErrno(epoll_ctl(EpollFds[ChainIndex][0], EPOLL_CTL_ADD, EpollFds[ChainIndex - 1][Index - 2], &EpollControlEvent));
    }

    //
    // Now try to introduce a loop into this extra-large chain.
    //

    LxtCheckErrnoFailure(epoll_ctl(EpollFds[0][0], EPOLL_CTL_ADD, EpollFds[1][0], &EpollControlEvent), ELOOP);

    LxtCheckErrnoFailure(epoll_ctl(EpollFds[0][0], EPOLL_CTL_ADD, EpollFds[EPOLL_CHAIN_COUNT - 1][Index - 1], &EpollControlEvent), ELOOP);

    //
    // The resulting chain is essentially useless. Try to add a file descriptor
    // to a node in the chain.
    //

    LxtCheckErrnoFailure(epoll_ctl(EpollFds[0][0], EPOLL_CTL_ADD, PipeFileDescriptors[0], &EpollControlEvent), EINVAL);

    LxtCheckErrnoFailure(epoll_ctl(EpollFds[EPOLL_CHAIN_COUNT - 1][0], EPOLL_CTL_ADD, PipeFileDescriptors[0], &EpollControlEvent), EINVAL);

    LxtCheckErrno(epoll_ctl(EpollFds[EPOLL_CHAIN_COUNT - 1][1], EPOLL_CTL_ADD, PipeFileDescriptors[0], &EpollControlEvent));

    LxtCheckErrnoFailure(epoll_ctl(EpollFds[EPOLL_CHAIN_COUNT - 1][0], EPOLL_CTL_ADD, PipeFileDescriptors[0], &EpollControlEvent), EINVAL);

    //
    // Verify the same file descriptor can be added to two different epolls
    // even when they are linked.
    //

    LxtCheckErrno(epoll_ctl(EpollFds[EPOLL_CHAIN_COUNT - 1][2], EPOLL_CTL_ADD, PipeFileDescriptors[0], &EpollControlEvent));

    //
    // Test if the really long chain can be waited on.
    //

    LxtCheckErrno(epoll_wait(EpollFds[EPOLL_CHAIN_COUNT - 1][Index - 1], &EpollWaitEvent, 1, 0));

    LxtCheckEqual(Result, 0, "%d");

    LxtCheckErrno(write(PipeFileDescriptors[1], "\n", 1));
    LxtCheckErrno(epoll_wait(EpollFds[EPOLL_CHAIN_COUNT - 1][Index - 1], &EpollWaitEvent, 1, -1));

    LxtCheckEqual(Result, 1, "%d");

    //
    // Try removing a node deep in the chain.
    //

    LxtCheckErrno(epoll_ctl(
        EpollFds[EPOLL_CHAIN_COUNT / 2][EPOLL_MAX_RECURSION_COUNT / 2],
        EPOLL_CTL_DEL,
        EpollFds[EPOLL_CHAIN_COUNT / 2][EPOLL_MAX_RECURSION_COUNT / 2 - 1],
        NULL));

ErrorExit:
    if (ExtraEpollFd != -1)
    {
        close(ExtraEpollFd);
    }

    for (Index = 0; Index < EPOLL_MAX_RECURSION_COUNT; Index += 1)
    {
        if (EpollContentFds[Index] != -1)
        {
            close(EpollContentFds[Index]);
        }
    }

    for (ChainIndex = 0; ChainIndex < EPOLL_CHAIN_COUNT; ChainIndex += 1)
    {
        for (Index = 0; Index < EPOLL_MAX_RECURSION_COUNT; Index += 1)
        {
            if (EpollFds[ChainIndex][Index] != -1)
            {
                close(EpollFds[ChainIndex][Index]);
            }
        }
    }

    if (PipeFileDescriptors[1] != -1)
    {
        close(PipeFileDescriptors[1]);
    }

    if (PipeFileDescriptors[0] != -1)
    {
        close(PipeFileDescriptors[0]);
    }

    return Result;
}