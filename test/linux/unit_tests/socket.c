/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    socket.c

Abstract:

    Linux socket client / server test.

--*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netdb.h>
#include "lxtcommon.h"
#include "unittests.h"
#include "common.h"

#define LXT_NAME_CLIENT "SocketClient"
#define LXT_NAME_SERVER "SocketServer"

#define LXT_AF_UNIX_SOCKET_PATH "af_unix_socket"

#define FD_STDOUT 1
//
// Function declarations.
//

long long GetTickCount(void);

int SocketClientDgram(PLXT_ARGS Args);

int SocketClientSend(int NumConnectAndSends, int Family, int Type, int Protocol);

int SocketClientSendMultiple(PLXT_ARGS Args);

int SocketClientSendMultipleIpv6(PLXT_ARGS Args);

int SocketClientSendWithFlags(PLXT_ARGS Args);

int SocketClientUnix(PLXT_ARGS Args);

int SocketCreateAcceptedSockets(int Family, int Type, int Protocol);

int SocketCreateBoundSocket(int Family, int Type, int Protocol);

int SocketCreateConnectSocket(int Family, int Type, int Protocol);

int SocketGetSockName(PLXT_ARGS Args);

int SocketParseCommandLine(int Argc, char* Argv[], LXT_ARGS* Args);

int SocketServerAccept(int NumAccepts, int Family, int Type, int Protocol);

int SocketServerAcceptMultiple(PLXT_ARGS Args);

int SocketServerAcceptMultipleIpv6(PLXT_ARGS Args);

int SocketServerAcceptWithFlags(PLXT_ARGS Args);

int SocketServerDgram(PLXT_ARGS Args);

int SocketServerUnix(PLXT_ARGS Args);

//
// Global constants.
//

static const LXT_VARIATION g_LxtClientVariations[] = {
    {"Socket Client - send multiple", SocketClientSendMultiple},
    {"Socket Client - AF_UNIX", SocketClientUnix},
    {"Socket Client - send multiple Ipv6", SocketClientSendMultipleIpv6},
    {"Socket Client - send (MSG_WAITALL)", SocketClientSendWithFlags},
    {"Socket Client - SOCK_DGRAM", SocketClientDgram},

    //
    // Variations that do not require a server.
    //

    {"Socket - getsockname", SocketGetSockName},
};

//
// N.B. Keep the number of variations up to date with
// LXT_SOCKET_NUM_SERVER_VARIATIONS in socket/common.h
//

static const LXT_VARIATION g_LxtServerVariations[] = {
    {"Socket Server - accept multiple", SocketServerAcceptMultiple},
    {"Socket Server - AF_UNIX", SocketServerUnix},
    {"Socket Server - accept multiple Ipv6", SocketServerAcceptMultipleIpv6},
    {"Socket Server - accept (MSG_WAITALL)", SocketServerAcceptWithFlags},
    {"Socket Server - SOCK_DGRAM", SocketServerDgram}};

//
// Function definitions.
//

long long GetTickCount(void)
{

    struct timespec Now;

    if (clock_gettime(CLOCK_MONOTONIC, &Now))
    {
        return 0;
    }

    return Now.tv_sec * 1000 + Now.tv_nsec / 1000000;
}

int main(int Argc, char* Argv[])

/*++
--*/

{

    LXT_ARGS Args;
    int Result;

    LxtCheckResult(SocketParseCommandLine(Argc, Argv, &Args))

        ErrorExit : LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int SocketClientDgram(PLXT_ARGS Args)
{

    char ReceiveBuffer[LXT_SOCKET_DEFAULT_BUFFER_LENGTH] = {0};
    int Result = LXT_RESULT_FAILURE;
    char* SendBuffer;
    struct sockaddr* ServerAddress = {0};
    struct sockaddr_in ServerAddressIpv4 = {0};
    struct sockaddr_in6 ServerAddressIpv6 = {0};
    struct sockaddr_un ServerAddressUnix = {0};
    int ServerAddressLength;
    int Size;
    int Socket = 0;

    sleep(2);

    Socket = socket(AF_INET6, SOCK_DGRAM, 0);
    if (Socket < 0)
    {
        LxtLogError("socket(AF_INET6, SOCK_DGRAM, 0) - %s", strerror(errno));
        goto ErrorExit;
    }

    ServerAddressIpv6.sin6_family = AF_INET6;
    ServerAddressIpv6.sin6_addr = in6addr_loopback;
    ServerAddressIpv6.sin6_port = htons(LXT_SOCKET_DEFAULT_PORT_IPV6);
    ServerAddress = (struct sockaddr*)&ServerAddressIpv6;
    ServerAddressLength = sizeof(ServerAddressIpv6);
    SendBuffer = LXT_SOCKET_DEFAULT_SEND_STRING;

    Size = sendto(Socket, SendBuffer, strlen(SendBuffer), 0, ServerAddress, ServerAddressLength);

    if (Size < 0)
    {
        LxtLogError("sendto - %s", strerror(errno));
        goto ErrorExit;
    }

    Size = recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0, ServerAddress, &ServerAddressLength);

    if (Size < 0)
    {
        LxtLogError("recvfrom - %s", strerror(errno));
        goto ErrorExit;
    }

    LxtLogInfo("Received from server: %s", ReceiveBuffer);

    if (memcmp(SendBuffer, ReceiveBuffer, Size) != 0)
    {
        LxtLogError("Message received back from server %s did not match expected %s", ReceiveBuffer, SendBuffer);

        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Socket > 0)
    {
        if (close(Socket) != 0)
        {
            LxtLogError("close(%d) - %s", Socket, strerror(errno));
        }
    }

    return Result;
}

int SocketClientSend(int NumConnectAndSends, int Family, int Type, int Protocol)

{

    int Index;
    char ReceiveBuffer[LXT_SOCKET_DEFAULT_BUFFER_LENGTH] = {0};
    int Result = LXT_RESULT_FAILURE;
    char* SendBuffer;
    int Size;
    int Socket = 0;

    //
    // Sleep to allow the server process to be listening.
    //

    sleep(2);
    SendBuffer = LXT_SOCKET_DEFAULT_SEND_STRING;
    for (Index = 0; Index < NumConnectAndSends; Index++)
    {
        Socket = SocketCreateConnectSocket(Family, Type, Protocol);
        if (Socket <= 0)
        {
            LxtLogError("SocketCreateConnectSocket failed");
            goto ErrorExit;
        }

        Size = send(Socket, SendBuffer, strlen(SendBuffer), 0);
        if (Size < 0)
        {
            LxtLogError("send(%d, SendBuffer, strlen(SendBuffer), 0) - %s", Socket, strerror(errno));

            goto ErrorExit;
        }

        memset(ReceiveBuffer, 0, sizeof(ReceiveBuffer));
        Size = read(Socket, ReceiveBuffer, Size);
        if (Size < 0)
        {
            LxtLogError("read(%d, ReceiveBuffer, sizeof(ReceiveBuffer) - %s", Socket, strerror(errno));

            goto ErrorExit;
        }

        LxtLogInfo("Received from server: %s", ReceiveBuffer);

        if (memcmp(SendBuffer, ReceiveBuffer, Size) != 0)
        {
            LxtLogError("Message received back from server %s did not match expected %s", ReceiveBuffer, SendBuffer);

            goto ErrorExit;
        }

        if (close(Socket) != 0)
        {
            LxtLogError("close(%d) - %s", Socket, strerror(errno));
        }

        Socket = 0;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Socket > 0)
    {
        if (close(Socket) != 0)
        {
            LxtLogError("close(%d) - %s", Socket, strerror(errno));
        }
    }

    return Result;
}

int SocketClientSendMultiple(PLXT_ARGS Args)

/*++
--*/

{

    return SocketClientSend(LXT_SOCKET_SERVER_MAX_BACKLOG_NUM, AF_INET, SOCK_STREAM, 0);
}

int SocketClientSendMultipleIpv6(PLXT_ARGS Args)

/*++
--*/

{

    return SocketClientSend(LXT_SOCKET_SERVER_MAX_BACKLOG_NUM, AF_INET6, SOCK_STREAM, 0);
}

int SocketClientSendWithFlags(PLXT_ARGS Args)
{
    int FullMessageSize;
    char* ReceiveBuffer;
    int Result = LXT_RESULT_FAILURE;
    char* SendBuffer;
    int Size;
    int Socket = 0;

    //
    // Sleep to allow the server process to be listening.
    //

    sleep(2);
    SendBuffer = LXT_SOCKET_DEFAULT_SEND_STRING;
    FullMessageSize = 2 * strlen(SendBuffer);
    ReceiveBuffer = malloc(FullMessageSize);
    if (ReceiveBuffer == NULL)
    {
        goto ErrorExit;
    }

    Socket = SocketCreateConnectSocket(AF_INET, SOCK_STREAM, 0);
    if (Socket <= 0)
    {
        goto ErrorExit;
    }

    Size = send(Socket, SendBuffer, strlen(SendBuffer), 0);
    if (Size < 0)
    {
        LxtLogError("send(%d, SendBuffer, strlen(SendBuffer), 0) - %s", Socket, strerror(errno));

        goto ErrorExit;
    }

    //
    // Sleep long enough that the second send won't be concatenated by WSK to
    // test MSW_WAITALL code path.
    //

    sleep(1);
    Size = send(Socket, SendBuffer, strlen(SendBuffer), 0);
    if (Size < 0)
    {
        LxtLogError("send(%d, SendBuffer, strlen(SendBuffer), 0) - %s", Socket, strerror(errno));

        goto ErrorExit;
    }

    memset(ReceiveBuffer, 0, FullMessageSize);
    Size = recv(Socket, ReceiveBuffer, FullMessageSize, MSG_WAITALL);
    if (Size < 0)
    {
        LxtLogError("read(%d, ReceiveBuffer, %d, MSG_WAITALL - %s", Socket, FullMessageSize, strerror(errno));

        goto ErrorExit;
    }

    LxtLogInfo("Received from server: %s", ReceiveBuffer);

    if (memcmp(SendBuffer, ReceiveBuffer, FullMessageSize / 2) != 0)
    {
        LxtLogError("Message received back from server %s did not match expected %s", ReceiveBuffer, SendBuffer);

        goto ErrorExit;
    }

    if (memcmp(SendBuffer, ReceiveBuffer + FullMessageSize / 2, sizeof(SendBuffer)) != 0)
    {

        LxtLogError("Message received back from server %s did not match expected %s", ReceiveBuffer, SendBuffer);

        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Socket > 0)
    {
        if (close(Socket) != 0)
        {
            LxtLogError("close(%d) - %s", Socket, strerror(errno));
        }
    }

    if (ReceiveBuffer != NULL)
    {
        free(ReceiveBuffer);
    }

    return Result;
}

int SocketClientUnix(PLXT_ARGS Args)
{

    return SocketClientSend(1, AF_UNIX, SOCK_SEQPACKET, 0);
}

int SocketCreateAcceptedSocket(int Socket, int Family)
{

    int AcceptedSocket = 0;
    int Result = LXT_RESULT_FAILURE;
    struct sockaddr* Address;
    struct sockaddr_in AddressIpv4 = {0};
    struct sockaddr_in6 AddressIpv6 = {0};
    struct sockaddr_un AddressUnix = {0};
    socklen_t AddressLength;

    switch (Family)
    {
    case AF_INET:
        Address = (struct sockaddr*)&AddressIpv4;
        AddressLength = sizeof(AddressIpv4);
        break;

    case AF_INET6:
        Address = (struct sockaddr*)&AddressIpv6;
        AddressLength = sizeof(AddressIpv6);
        break;

    case AF_UNIX:
        Address = (struct sockaddr*)&AddressUnix;
        AddressLength = sizeof(AddressUnix);
        break;

    default:
        LxtLogError("Unsupported Family %d", Family);
        AcceptedSocket = 0;
        goto ErrorExit;
    }

    AcceptedSocket = accept(Socket, Address, &AddressLength);

    if (AcceptedSocket < 0)
    {
        LxtLogError("accept(%d, Address, &AddressLength) - %s", Socket, strerror(errno));

        close(AcceptedSocket);
        goto ErrorExit;
    }

    Result = AcceptedSocket;

ErrorExit:
    return Result;
}

int SocketCreateBoundSocket(int Family, int Type, int Protocol)
{
    int OptVal;
    int Result = LXT_RESULT_FAILURE;
    struct sockaddr* ServerAddress;
    int ServerAddressSize;
    struct sockaddr_in ServerAddressIpv4 = {0};
    struct sockaddr_in6 ServerAddressIpv6 = {0};
    struct sockaddr_un ServerAddressUnix = {0};
    int Size;
    int Socket = 0;

    Socket = socket(Family, Type, Protocol);
    if (Socket < 0)
    {
        LxtLogError("socket(%d, %d, %d) - %s", Family, Type, Protocol, strerror(errno));
        goto ErrorExit;
    }

    //
    // TODO: when setsockopt is implemented uncomment the below.
    //
    // OptVal = 1;
    // if (setsockopt(Socket, SOL_SOCKET, SO_REUSEADDR, &OptVal, sizeof(OptVal)) < 0) {
    //    LxtLogError("setsockopt(%d, SOL_SOCKET, SO_REUSEADDR, &OptVal, sizeof(OptVal)) - %s",
    //                 Socket,
    //                 strerror(errno));
    //    goto ErrorExit;
    //}

    switch (Family)
    {
    case AF_INET:
        ServerAddressIpv4.sin_family = AF_INET;
        ServerAddressIpv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ServerAddressIpv4.sin_port = htons(LXT_SOCKET_DEFAULT_PORT);
        ServerAddress = (struct sockaddr*)&ServerAddressIpv4;
        ServerAddressSize = sizeof(ServerAddressIpv4);
        break;

    case AF_INET6:
        ServerAddressIpv6.sin6_family = AF_INET6;
        ServerAddressIpv6.sin6_addr = in6addr_loopback;
        ServerAddressIpv6.sin6_port = htons(LXT_SOCKET_DEFAULT_PORT_IPV6);
        ServerAddress = (struct sockaddr*)&ServerAddressIpv6;
        ServerAddressSize = sizeof(ServerAddressIpv6);
        break;

    case AF_UNIX:
        ServerAddressUnix.sun_family = AF_UNIX;
        strcpy(ServerAddressUnix.sun_path, LXT_AF_UNIX_SOCKET_PATH);
        ServerAddress = (struct sockaddr*)&ServerAddressUnix;
        ServerAddressSize = sizeof(ServerAddressUnix);
        // unlink(LXT_AF_UNIX_SOCKET_PATH); // TODO: when unlink is implemented uncomment this.
        break;

    default:
        LxtLogError("Unsupported Family %d", Family);
        close(Socket);
        Socket = 0;
        goto ErrorExit;
    }

    if (bind(Socket, ServerAddress, ServerAddressSize) < 0)
    {
        LxtLogError("bind(%d, ServerAddress, ServerAddressSize) - %s", Socket, strerror(errno));

        close(Socket);
        Socket = 0;
        goto ErrorExit;
    }

    Result = Socket;

ErrorExit:
    return Result;
}

int SocketCreateConnectSocket(int Family, int Type, int Protocol)
{

    int Result = LXT_RESULT_FAILURE;
    struct sockaddr* ServerAddress = NULL;
    struct sockaddr_in ServerAddressIpv4 = {0};
    struct sockaddr_in6 ServerAddressIpv6 = {0};
    int ServerAddressSize;
    struct sockaddr_un ServerAddressUnix = {0};
    int Size;
    int Socket = 0;

    Socket = socket(Family, Type, Protocol);
    if (Socket < 0)
    {
        LxtLogError("socket(%d, %d, %d) - %s", Family, Type, Protocol, strerror(errno));
        goto ErrorExit;
    }

    switch (Family)
    {
    case AF_INET:
        ServerAddressIpv4.sin_family = AF_INET;
        ServerAddressIpv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ServerAddressIpv4.sin_port = htons(LXT_SOCKET_DEFAULT_PORT);
        ServerAddress = (struct sockaddr*)&ServerAddressIpv4;
        ServerAddressSize = sizeof(ServerAddressIpv4);
        break;

    case AF_INET6:
        ServerAddressIpv6.sin6_family = AF_INET6;
        ServerAddressIpv6.sin6_addr = in6addr_loopback;
        ServerAddressIpv6.sin6_port = htons(LXT_SOCKET_DEFAULT_PORT_IPV6);
        ServerAddress = (struct sockaddr*)&ServerAddressIpv6;
        ServerAddressSize = sizeof(ServerAddressIpv6);
        break;

    case AF_UNIX:
        ServerAddressUnix.sun_family = AF_UNIX;
        strcpy(ServerAddressUnix.sun_path, LXT_AF_UNIX_SOCKET_PATH);
        ServerAddress = (struct sockaddr*)&ServerAddressUnix;
        ServerAddressSize = sizeof(ServerAddressUnix);
        break;

    default:
        LxtLogError("LxtSocketClientSend Unsupported Family %d", Family);
        close(Socket);
        Socket = 0;
        goto ErrorExit;
    }

    if (connect(Socket, ServerAddress, ServerAddressSize) < 0)
    {
        LxtLogError("connect failed - %s", strerror(errno));
        close(Socket);
        Socket = 0;
        goto ErrorExit;
    }

    Result = Socket;

ErrorExit:
    return Result;
}

int SocketGetSockName(PLXT_ARGS Args)
{
    union
    {
        struct in_addr Addr;
        struct in6_addr AddrIpv6;
    } Addr;
    struct sockaddr_storage Address = {0};
    int AddressFamilies[] = {AF_INET, AF_INET6};
    int AddressFamily;
    int AddressSize;
    int Port;
    int Result = LXT_RESULT_FAILURE;
    int Socket = 0;
    int Index;

    for (Index = 0; Index < (int)LXT_COUNT_OF(AddressFamilies); Index++)
    {
        Socket = socket(AddressFamilies[Index], SOCK_STREAM, 0);
        if (Socket < 0)
        {
            LxtLogError("socket(%d, SOCK_STREAM, 0) - %s", AddressFamilies[Index], strerror(errno));

            goto ErrorExit;
        }

        switch (AddressFamilies[Index])
        {
        case AF_INET:
            ((struct sockaddr_in*)&Address)->sin_family = AF_INET;
            ((struct sockaddr_in*)&Address)->sin_port = htons(0);
            ((struct sockaddr_in*)&Address)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            AddressSize = sizeof(struct sockaddr_in);
            break;

        case AF_INET6:
            ((struct sockaddr_in6*)&Address)->sin6_family = AF_INET6;
            ((struct sockaddr_in6*)&Address)->sin6_port = htons(0);
            ((struct sockaddr_in6*)&Address)->sin6_addr = in6addr_loopback;
            AddressSize = sizeof(struct sockaddr_in6);
            break;

        default:
            LxtLogError("Unsupported Family %d", AddressFamilies[Index]);
            goto ErrorExit;
        }

        if (bind(Socket, (struct sockaddr*)&Address, AddressSize) < 0)
        {
            LxtLogError("bind(%d, (struct sockaddr*)&Address, AddressSize) - %s", Socket, strerror(errno));

            goto ErrorExit;
        }

        if (getsockname(Socket, (struct sockaddr*)&Address, &AddressSize) < 0)
        {
            LxtLogError("getsockname(%d, &Address, &AddressSize) - %s", Socket, strerror(errno));

            goto ErrorExit;
        }

        memset(&Addr, 0, sizeof(Addr));
        switch (AddressFamilies[Index])
        {
        case AF_INET:
            AddressFamily = ((struct sockaddr_in*)&Address)->sin_family;
            Port = ((struct sockaddr_in*)&Address)->sin_port;
            memcpy(
                (struct sockaddr_in*)&Addr,
                &((struct sockaddr_in*)&Address)->sin_addr.s_addr,
                sizeof(((struct sockaddr_in*)&Address)->sin_addr.s_addr));

            break;

        case AF_INET6:
            AddressFamily = ((struct sockaddr_in6*)&Address)->sin6_family;
            Port = ((struct sockaddr_in6*)&Address)->sin6_port;
            memcpy(
                (struct sockaddr_in6*)&Addr, &((struct sockaddr_in6*)&Address)->sin6_addr, sizeof(((struct sockaddr_in6*)&Address)->sin6_addr));

            break;

        default:
            LxtLogError("Unsupported AddressFamily %d", AddressFamilies[Index]);
            goto ErrorExit;
        }

        if (AddressFamily != AddressFamilies[Index])
        {
            LxtLogError("Socket %d is bound, address family %d should be %d", Socket, AddressFamily, AddressFamilies[Index], strerror(errno));

            goto ErrorExit;
        }

        if (Port == 0)
        {
            LxtLogError("Socket %d is bound, port should be non-null", Socket, strerror(errno));

            goto ErrorExit;
        }

        //
        // Create the underlaying socket and query again, port should be the same.
        //

        if (listen(Socket, 32) < 0)
        {
            LxtLogError("listen(%d, 32) - %s", Socket, strerror(errno));
            goto ErrorExit;
        }

        if (getsockname(Socket, (struct sockaddr*)&Address, &AddressSize) < 0)
        {
            LxtLogError("getsockname(%d, &Address, &AddressSize) - %s", Socket, strerror(errno));

            goto ErrorExit;
        }

        switch (AddressFamilies[Index])
        {
        case AF_INET:
            if (AddressFamily != ((struct sockaddr_in*)&Address)->sin_family)
            {
                LxtLogError("Socket %d is bound, address family %d should be %d", Socket, ((struct sockaddr_in*)&Address)->sin_family, AddressFamily, strerror(errno));

                goto ErrorExit;
            }

            if (Port != ((struct sockaddr_in*)&Address)->sin_port)
            {
                LxtLogError("Socket %d should be bound to port %d", Socket, Port, strerror(errno));

                goto ErrorExit;
            }

            if (memcmp(&Addr, &((struct sockaddr_in*)&Address)->sin_addr.s_addr, sizeof(((struct sockaddr_in*)&Address)->sin_addr.s_addr)) != 0)
            {

                LxtLogError("Socket %d addr should be localhost", Socket, strerror(errno));

                goto ErrorExit;
            }

            break;

        case AF_INET6:
            if (AddressFamily != ((struct sockaddr_in6*)&Address)->sin6_family)
            {
                LxtLogError(
                    "Socket %d is bound, address family %d should be %d",
                    Socket,
                    ((struct sockaddr_in6*)&Address)->sin6_family,
                    AddressFamily,
                    strerror(errno));

                goto ErrorExit;
            }

            if (Port != ((struct sockaddr_in6*)&Address)->sin6_port)
            {
                LxtLogError("Socket %d should be bound to port %d", Socket, Port, strerror(errno));

                goto ErrorExit;
            }

            if (memcmp(&Addr, &((struct sockaddr_in6*)&Address)->sin6_addr, sizeof(((struct sockaddr_in6*)&Address)->sin6_addr)) != 0)
            {

                LxtLogError("Socket %d addr should be localhost", Socket, strerror(errno));

                goto ErrorExit;
            }
            break;

        default:
            LxtLogError("Unsupported Family %d", AddressFamilies[Index]);
            goto ErrorExit;
        }

        if (close(Socket) != 0)
        {
            LxtLogError("close(%d) - %s", Socket, strerror(errno));
        }

        Socket = 0;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Socket > 0)
    {
        if (close(Socket) != 0)
        {
            LxtLogError("close(%d) - %s", Socket, strerror(errno));
        }
    }

    return Result;
}

int SocketParseCommandLine(int Argc, char* Argv[], LXT_ARGS* Args)

/*++
--*/

{

    int ArgvIndex;
    int Result;
    int ValidArguments;

    Result = LXT_RESULT_FAILURE;
    ValidArguments = 0;

    if (Argc < 2)
    {
        goto ErrorExit;
    }

    for (ArgvIndex = 1; ArgvIndex < Argc; ++ArgvIndex)
    {
        if (Argv[ArgvIndex][0] != '-')
        {
            printf("Unexpected character %s\n", Argv[ArgvIndex]);
            goto ErrorExit;
        }

        switch (Argv[ArgvIndex][1])
        {
        case 'c':

            //
            // Run client variations.
            //

            ValidArguments = 1;
            LxtCheckResult(LxtInitialize(Argc, Argv, Args, LXT_NAME_CLIENT));
            LxtCheckResult(LxtRunVariations(Args, g_LxtClientVariations, LXT_COUNT_OF(g_LxtClientVariations)));
            break;

        case 's':

            //
            // Run server variations.
            //

            ValidArguments = 1;
            LxtCheckResult(LxtInitialize(Argc, Argv, Args, LXT_NAME_SERVER));
            LxtCheckResult(LxtRunVariations(Args, g_LxtServerVariations, LXT_COUNT_OF(g_LxtServerVariations)));
            break;

        case 'v':

            //
            // This was already taken care of by LxtInitialize.
            //

            ++ArgvIndex;

            break;

        default:
            goto ErrorExit;
        }
    }

ErrorExit:
    if (ValidArguments == 0)
    {
        printf("\nuse: socket <One of the below arguments>\n");
        printf("\t-c : Run all client variations (server must already be running)\n");
        printf("\t-s : Run all server variations\n");
    }

    return Result;
}

int SocketServerDgram(PLXT_ARGS Args)
{

    char Buffer[LXT_SOCKET_DEFAULT_BUFFER_LENGTH] = {0};
    struct sockaddr* FromAddress = {0};
    struct sockaddr_in FromAddressIpv4 = {0};
    struct sockaddr_in6 FromAddressIpv6 = {0};
    struct sockaddr_un FromAddressUnix = {0};
    int FromAddressLength;
    int Result = LXT_RESULT_FAILURE;
    int Size;
    int Socket = 0;

    Socket = SocketCreateBoundSocket(AF_INET6, SOCK_DGRAM, 0);
    if (Socket < 0)
    {
        goto ErrorExit;
    }

    FromAddress = (struct sockaddr*)&FromAddressIpv6;
    FromAddressLength = sizeof(FromAddressIpv6);

    Size = recvfrom(Socket, Buffer, sizeof(Buffer), 0, FromAddress, &FromAddressLength);

    if (Size < 0)
    {
        LxtLogError("recvfrom - %s", strerror(errno));
        goto ErrorExit;
    }

    LxtLogInfo("Received : %s", Buffer);

    Size = sendto(Socket, Buffer, Size, 0, FromAddress, FromAddressLength);

    if (Size < 0)
    {
        LxtLogError("sendto - %s", strerror(errno));
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Socket > 0)
    {
        if (close(Socket) != 0)
        {
            LxtLogError("close(%d) - %s", Socket, strerror(errno));
        }
    }

    return Result;
}

int SocketServerAccept(int NumAccepts, int Family, int Type, int Protocol)

/*++
--*/

{

    char Buffer[LXT_SOCKET_DEFAULT_BUFFER_LENGTH] = {0};
    int ClientSockets[LXT_SOCKET_SERVER_MAX_BACKLOG_NUM] = {0};
    int Index;
    int Result = LXT_RESULT_FAILURE;
    int Size;
    int Socket = 0;

    Socket = SocketCreateBoundSocket(Family, Type, Protocol);
    if (Socket < 0)
    {
        goto ErrorExit;
    }

    if (listen(Socket, NumAccepts) < 0)
    {
        LxtLogError("listen(%d, %d) - %s", Socket, NumAccepts, strerror(errno));
        goto ErrorExit;
    }

    for (Index = 0; Index < NumAccepts; Index++)
    {
        ClientSockets[Index] = SocketCreateAcceptedSocket(Socket, Family);
        if (ClientSockets[Index] < 0)
        {
            goto ErrorExit;
        }

        memset(Buffer, 0, sizeof(Buffer));
        Size = read(ClientSockets[Index], Buffer, sizeof(Buffer));

        //
        // TODO: might need to handle the case where the socket gets shut down
        // without receiving any data as in the connect / close variation.
        //

        if (Size < 0)
        {
            LxtLogError("read(%d, Buffer, sizeof(Buffer)) - %s", ClientSockets[Index], strerror(errno));
            goto ErrorExit;
        }

        LxtLogInfo("Received: %s", Buffer);

        if (write(ClientSockets[Index], Buffer, Size) < 0)
        {
            LxtLogError("write(%d, Buffer, Size) - %s", ClientSockets[Index], strerror(errno));
            goto ErrorExit;
        }
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    for (Index = 0; Index < NumAccepts; Index++)
    {
        if (ClientSockets[Index] > 0)
        {
            if (close(ClientSockets[Index]) < 0)
            {
                LxtLogError("close(%d) - %s", ClientSockets[Index], strerror(errno));
            }
        }
    }

    if (Socket > 0)
    {
        if (close(Socket) != 0)
        {
            LxtLogError("close(%d) - %s", Socket, strerror(errno));
        }
    }

    return Result;
}

int SocketServerAcceptMultiple(PLXT_ARGS Args)

/*++
--*/

{

    return SocketServerAccept(LXT_SOCKET_SERVER_MAX_BACKLOG_NUM, AF_INET, SOCK_STREAM, 0);
}

int SocketServerAcceptMultipleIpv6(PLXT_ARGS Args)

/*++
--*/

{

    return SocketServerAccept(LXT_SOCKET_SERVER_MAX_BACKLOG_NUM, AF_INET6, SOCK_STREAM, 0);
}

int SocketServerAcceptWithFlags(PLXT_ARGS Args)
{
    int AcceptedSocket = 0;
    int Index;
    char* ReceiveBuffer;
    int Result = LXT_RESULT_FAILURE;
    int Size;
    int FullMessageSize;
    int Socket = 0;

    ReceiveBuffer = LXT_SOCKET_DEFAULT_SEND_STRING;
    FullMessageSize = 2 * strlen(ReceiveBuffer);
    ReceiveBuffer = malloc(FullMessageSize);
    if (ReceiveBuffer == NULL)
    {
        goto ErrorExit;
    }

    Socket = SocketCreateBoundSocket(AF_INET, SOCK_STREAM, 0);
    if (Socket < 0)
    {
        goto ErrorExit;
    }

    if (listen(Socket, 32) < 0)
    {
        LxtLogError("listen(%d, 32) - %s", Socket, strerror(errno));
        goto ErrorExit;
    }

    AcceptedSocket = SocketCreateAcceptedSocket(Socket, AF_INET);
    memset(ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    Size = recv(AcceptedSocket, ReceiveBuffer, FullMessageSize, MSG_WAITALL);

    //
    // TODO: might need to handle the case where the socket gets shut down
    // without receiving any data as in the connect / close variation.
    //

    if (Size < 0)
    {
        LxtLogError("recv(%d, Buffer, %d, MSG_WAITALL) - %s", AcceptedSocket, FullMessageSize, strerror(errno));
        goto ErrorExit;
    }

    LxtLogInfo("Received: %s", ReceiveBuffer);

    if (write(AcceptedSocket, ReceiveBuffer, Size) < 0)
    {
        LxtLogError("write(%d, Buffer, Size) - %s", AcceptedSocket, strerror(errno));
        goto ErrorExit;
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (AcceptedSocket > 0)
    {
        if (close(AcceptedSocket) < 0)
        {
            LxtLogError("close(%d) - %s", AcceptedSocket, strerror(errno));
        }
    }

    if (Socket > 0)
    {
        if (close(Socket) != 0)
        {
            LxtLogError("close(%d) - %s", Socket, strerror(errno));
        }
    }

    if (ReceiveBuffer != NULL)
    {
        free(ReceiveBuffer);
    }

    return Result;
}

int SocketServerUnix(PLXT_ARGS Args)
{

    return SocketServerAccept(1, AF_UNIX, SOCK_SEQPACKET, 0);
}