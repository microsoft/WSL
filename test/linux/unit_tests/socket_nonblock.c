/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    socket_nonblock.c

Abstract:

    This file is a simple test for nonblocking sockets.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netdb.h>
#include "common.h"

#include <sys/wait.h>

#define LXT_NAME "socket_nonblocking"

int SocketAsyncTest(PLXT_ARGS Args);

#define SOCKET_NAME "PartyInTheUsa"

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Socket_Async_Simple", SocketAsyncTest},
};

int SocketNonblockTestEntry(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Result;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));
    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

#ifndef EPOLLONESHOT
#define EPOLLONESHOT (1 << 30)
#endif

int NonblockEpollCreateClientSocket(int IsNonBlocking)

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

    //
    // Set the socket as non blocking.
    //

    if (IsNonBlocking != 0)
    {
        Result = fcntl(Socket, F_SETFL, O_NONBLOCK);
        if (Result < 0)
        {
            LxtLogError("fcntl(%d) - %d", Socket, errno);
            Result = -1;
            goto cleanup;
        }
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

int NonblockEpollCreateClientUnixSocket(int IsNonBlocking)

/*++
--*/

{
    int Result;
    struct sockaddr_un ServerAddress = {0};
    int Socket;

    //
    // Create a socket.
    //

    Socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (Socket < 0)
    {
        LxtLogError("socket(AF_UNIX, SOCK_STREAM, 0) - %s", strerror(errno));
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

    //
    // Set the socket as non blocking.
    //

    if (IsNonBlocking != 0)
    {
        Result = fcntl(Socket, F_SETFL, O_NONBLOCK);
        if (Result < 0)
        {
            LxtLogError("fcntl(%d) - %d", Socket, errno);
            Result = -1;
            goto cleanup;
        }
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

int NonblockEpollCreateListenSocket(int IsNonBlocking)

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

    //
    // Set the socket as non blocking.
    //

    if (IsNonBlocking != 0)
    {
        Result = fcntl(Socket, F_SETFL, O_NONBLOCK);
        if (Result < 0)
        {
            LxtLogError("fcntl(%d) - %d", Socket, errno);
            Result = -1;
            goto cleanup;
        }
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

int NonblockEpollCreateListenUnixSocket(int IsNonBlocking)

/*++
--*/

{

    struct sockaddr_un ServerAddress = {0};
    int Result;
    int Socket;

    //
    // Create a socket.
    //

    Socket = socket(AF_UNIX, SOCK_STREAM, 0);
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

    //
    // Set the socket as non blocking.
    //

    if (IsNonBlocking != 0)
    {
        Result = fcntl(Socket, F_SETFL, O_NONBLOCK);
        if (Result < 0)
        {
            LxtLogError("fcntl(%d) - %d", Socket, errno);
            Result = -1;
            goto cleanup;
        }
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
int NonblockEpollHandleClientAccept(int Socket)

/*++
--*/

{

    struct sockaddr_in ClientAddress = {0};
    socklen_t ClientLength;
    int Error;
    int Result;
    int RetryCount;

    ClientLength = sizeof(ClientAddress);
    Result = -1;
    RetryCount = 0;

    while (1)
    {
        Result = accept(Socket, (struct sockaddr*)&ClientAddress, &ClientLength);
        if (Result < 0)
        {
            Error = errno;
            LxtLogInfo("[Server] accept(%d) returned %d (error %d)", Socket, Result, Error);

            if (Error == EAGAIN)
            {
                LxtLogInfo("[Server] nonblocking accept said try again, sleeping...");
                usleep(1 * 1000 * 1000);

                RetryCount += 1;
                if (RetryCount > 10)
                {
                    LxtLogInfo("[Server] too many retries, exiting...");
                    Result = -1;
                    goto cleanup;
                }

                continue;
            }
        }

        break;
    }

cleanup:

    return Result;
}

const char* NonblockDataToWrite[] = {
    "<This is the first message> ",
    "<This is another message> ",
    "<Dumbledore is dead> ",
    "<Harry Potter must not go back to Hogwarts> ",
    "<There must always be a stark in Winterfell>",
};

const int NonblockWriteItemCount = sizeof(NonblockDataToWrite) / sizeof(NonblockDataToWrite[0]);

int SocketAsyncTest(PLXT_ARGS Args)

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
    int RetryCount;

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

    FileDescriptor1 = NonblockEpollCreateListenSocket(1);
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

        LxtLogInfo("[Client] Waiting 5 seconds to let server block...");

        usleep(5 * 1000 * 1000);

        LxtLogInfo("[Client] Connecting to server...");

        FileDescriptor2 = NonblockEpollCreateClientSocket(1);
        if (FileDescriptor2 == -1)
        {
            Result = errno;
            LxtLogError("[Client] Could not connect to server! %d", Result);
            goto cleanup;
        }

        LxtLogInfo("[Client] Connected to server, fd = %d", FileDescriptor2);

        //
        // Wait for data to be available in a loop.
        //

        RetryCount = 0;
        while (1)
        {

            LxtLogInfo("[Client] Trying to read data ...");

            memset(Buffer, 0, sizeof(Buffer));

            Result = read(FileDescriptor2, Buffer, sizeof(Buffer));
            if (Result < 0)
            {
                Result = errno;
                if (Result = EAGAIN)
                {
                    LxtLogInfo("[Client] No data available, will try again...");
                    usleep(1 * 1000 * 1000);

                    RetryCount += 1;
                    if (RetryCount > 10)
                    {
                        LxtLogInfo("[Client] Too many retries, exiting...");
                        Result = -1;

                        goto cleanup;
                    }

                    continue;
                }

                Result = -1;
                goto cleanup;
            }

            LxtLogInfo("[Client] read %d bytes: %s ...", Result, Buffer);

            if (Result == 0)
            {
                LxtLogInfo("[Client] exiting after reading 0 bytes ...");
                goto cleanup;
            }

            //
            // Reset the retry count after a successful read.
            //

            RetryCount = 0;
        }
    }

    //
    // Accept an incoming connection.
    //

    FileDescriptor2 = NonblockEpollHandleClientAccept(FileDescriptor1);
    if (FileDescriptor2 == -1)
    {
        Result = errno;
        LxtLogError("[Server] Could not accept! %d", Result);
        goto cleanup;
    }

    LxtLogInfo("[Server] Writing to socket %d times!", NonblockWriteItemCount);

    for (Index = 0; Index < NonblockWriteItemCount; Index += 1)
    {

        Result = write(FileDescriptor2, NonblockDataToWrite[Index], strlen(NonblockDataToWrite[Index]));

        if (Result < 0)
        {
            LxtLogError("[Server] Write %d failed %d", Index, Result);

            goto cleanup;
        }

        LxtLogInfo(
            "[Server] Write (%d, %s, %d) -> %d!",
            FileDescriptor2,
            NonblockDataToWrite[Index],
            strlen(NonblockDataToWrite[Index]) + (Index == NonblockWriteItemCount - 1),
            Result);

        usleep(1 * 1000 * 1000);
    }

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
