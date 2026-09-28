/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    common.c

Abstract:

    Common socket definitions and helper routines.

--*/

#include <stdlib.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <stdbool.h>
#include "common.h"
#include "lxtcommon.h"

int LxtSocketEpoll(int Descriptor, int Event, int Timeout)

/*++

Routine Description:

    This routine checks whether the given epoll is set in the file descriptor.

Arguments:

    Descriptor - Supplies the descriptor.

    Event - Supplies an event to check for.

    Timeout - Supplies the timeout value in milliseconds.

Return Value:

    0 on success, -1 on failure.

--*/

{

    struct epoll_event EpollEvent;
    int EpollFd;
    int Iterator;
    int NumberDescriptors;
    int Result;

    EpollFd = -1;
    LxtCheckErrno(EpollFd = epoll_create(1));
    EpollEvent.events = Event;
    EpollEvent.data.fd = Descriptor;
    LxtCheckErrno(epoll_ctl(EpollFd, EPOLL_CTL_ADD, Descriptor, &EpollEvent));
    LxtCheckErrno(NumberDescriptors = epoll_wait(EpollFd, &EpollEvent, 1, Timeout));

    //
    // If no descriptors were ready within the timeout, that is an error condition
    //

    if (NumberDescriptors != 1)
    {
        LxtLogInfo("expecting epoll_wait to return 1, but it returned %d", NumberDescriptors);

        Result = -1;
        errno = EAGAIN;
        goto ErrorExit;
    }

    if ((EpollEvent.events & Event) == 0)
    {
        LxtLogError("epoll event(%d) is not set. Epoll event(s) set: %d", Event, EpollEvent.events);

        Result = -1;
        errno = EINVAL;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (EpollFd != -1)
    {
        close(EpollFd);
    }

    return Result;
}

void* SocketBlockedReaderThread(void* Arg)

/*++

Routine Description:

    This routine will call read on the given fd and block.

Arguments:

    Arg - Supplies the argument for the datagram server to operate.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[10] = "123456789";
    ssize_t BytesRead;
    int Fd;
    int Result = LXT_RESULT_FAILURE;
    Fd = *(int*)Arg;
    LxtCheckErrno(BytesRead = recv(Fd, Buffer, sizeof(Buffer), 0));
    if (BytesRead != 0)
    {
        LxtLogError("recv should return 0 bytes read, but it returned %d bytes", BytesRead);

        goto ErrorExit;
    }

    LxtLogInfo("recv unblocked");
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    pthread_exit((void*)(ssize_t)Result);
}

void* SocketBlockedReaderZeroBufferThread(void* Arg)

/*++

Routine Description:

    This routine will call read on the given fd with a zero-byte receive buffer
    and block.

Arguments:

    Arg - Supplies the argument for the datagram server to operate.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[10] = "123456789";
    ssize_t BytesRead;
    int Fd;
    int Result = LXT_RESULT_FAILURE;
    Fd = *(int*)Arg;
    LxtCheckErrno(BytesRead = recv(Fd, Buffer, 0, 0));
    if (BytesRead != 0)
    {
        LxtLogError("recv should return 0 bytes read, but it returned %d bytes", BytesRead);

        goto ErrorExit;
    }

    LxtLogInfo("recv unblocked");
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    pthread_exit((void*)(ssize_t)Result);
}

struct cmsghdr* SocketGetControlMessage(struct msghdr* MessageHeader, struct cmsghdr* StartControlMessage, int Level, int Type)

/*++

Routine Description:

    This routine will return the control information at the given level and
    type.

Arguments:

    MessageHeader - Supplies the message header from which the control
        information has to be extracted.

    StartControlMessage - Supplies the control message from where to start.
        NULL if the search has to start from the beginning.

    Level - Supplies the level of the control information.

    Type - Supplies the type of the control information.

Return Value:

    Control message or NULL if it does not exist.

--*/

{

    struct cmsghdr* ControlMessage;

    //
    // Use the system macros to extract receive the ip packet control info.
    //

    ControlMessage = NULL;

    //
    // If the start control message is provided use that, else get the first
    // control message. This is automatically handled by MY_CMSG_NXTHDR.
    //

    for (ControlMessage = MY_CMSG_NXTHDR(MessageHeader, StartControlMessage); ControlMessage != NULL;
         ControlMessage = MY_CMSG_NXTHDR(MessageHeader, ControlMessage))
    {

        if (ControlMessage->cmsg_len < sizeof(struct cmsghdr))
        {
            break;
        }

        //
        // Look for a match.
        //

        if ((ControlMessage->cmsg_level == Level) && (ControlMessage->cmsg_type == Type))
        {

            return ControlMessage;
        }
    }

    return NULL;
}

void* SocketBlockedWriterThread(void* Arg)

/*++

Routine Description:

    This routine will call write on the given fd and block.

Arguments:

    Arg - Supplies the argument for the datagram server to operate.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char Buffer[10] = "123456789";
    ssize_t BytesWritten;
    int Fd;
    int Result = LXT_RESULT_FAILURE;
    Fd = *(int*)Arg;
    LxtCheckErrnoFailure(send(Fd, Buffer, sizeof(Buffer), 0), EPIPE);
    LxtLogInfo("send unblocked");
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    pthread_exit((void*)(ssize_t)Result);
}

int SocketGetSetBooleanSocketOption(int Socket, int OptionLevel, int OptionName, bool SmallerSizeAllowed)

/*++

Routine Description:

    This routine tests the getsockopt() and setsockopt() API for the
    any of the boolean socket option.

Arguments:

    Socket - Supplies the socket.

    OptionLevel - Supplies the level at which the option has to be applied.

    Option - Supplies the option to test.

    SmallerSizeAllowed - Supplies a boolean indicating whether sizes smaller
        than the size of the option are allowed.

Return Value:

    Returns 0 on success, -1 on failure.

--*/
{

    int Result;
    int Option;
    socklen_t OptionLength;
    long long int OptionLong;

    Result = LXT_RESULT_FAILURE;

    //
    // Validate proper handling of boolean socket options.
    //

    Option = 1;
    OptionLength = sizeof(Option);
    LxtCheckErrno(setsockopt(Socket, OptionLevel, OptionName, &Option, OptionLength));

    Option = 0;
    OptionLength = sizeof(Option);
    LxtCheckErrno(getsockopt(Socket, OptionLevel, OptionName, &Option, &OptionLength));

    LxtCheckEqual(Option, 1, "%d");

    //
    // Reset the option value to 0.
    //

    Option = 0;
    OptionLength = sizeof(Option);
    LxtCheckErrno(setsockopt(Socket, OptionLevel, OptionName, &Option, OptionLength));

    Option = 0;
    OptionLength = sizeof(Option);
    LxtCheckErrno(getsockopt(Socket, OptionLevel, OptionName, &Option, &OptionLength));

    LxtCheckEqual(Option, 0, "%d");

    //
    // Since it is a boolean option, any value other than 0 is accepted for
    // enabling the option. Try -ve.
    //

    Option = -1;
    OptionLength = sizeof(Option);
    LxtCheckErrno(setsockopt(Socket, OptionLevel, OptionName, &Option, OptionLength));

    Option = 0;
    OptionLength = sizeof(Option);
    LxtCheckErrno(getsockopt(Socket, OptionLevel, OptionName, &Option, &OptionLength));

    LxtCheckEqual(Option, 1, "%d");

    //
    // Reset the option value to 0.
    //

    Option = 0;
    OptionLength = sizeof(Option);
    LxtCheckErrno(setsockopt(Socket, OptionLevel, OptionName, &Option, OptionLength));

    Option = 0;
    OptionLength = sizeof(Option);
    LxtCheckErrno(getsockopt(Socket, OptionLevel, OptionName, &Option, &OptionLength));

    LxtCheckEqual(Option, 0, "%d");

    //
    // Since it is a boolean option, any value other than 0 is accepted for
    // enabling the option. Try > 0.
    //

    Option = 15;
    OptionLength = sizeof(Option);
    LxtCheckErrno(setsockopt(Socket, OptionLevel, OptionName, &Option, OptionLength));

    LxtCheckErrno(getsockopt(Socket, OptionLevel, OptionName, &Option, &OptionLength));

    LxtCheckEqual(Option, 1, "%d");

    if (SmallerSizeAllowed == false)
    {

        //
        // Validate that also 1,2 and 3 byte size is not a valid option size for
        // boolean socket option.
        //

        OptionLength = 1;
        LxtCheckErrnoFailure(setsockopt(Socket, OptionLevel, OptionName, &Option, OptionLength), EINVAL);

        OptionLength = 2;
        LxtCheckErrnoFailure(setsockopt(Socket, OptionLevel, OptionName, &Option, OptionLength), EINVAL);

        OptionLength = 3;
        LxtCheckErrnoFailure(setsockopt(Socket, OptionLevel, OptionName, &Option, OptionLength), EINVAL);

        OptionLength = sizeof(Option);
        LxtCheckErrno(getsockopt(Socket, OptionLevel, OptionName, &Option, &OptionLength));

        LxtCheckEqual(Option, 1, "%d");
    }
    else
    {

        //
        // Supplying an option size of 1, 2 and 3 are also accepted.
        //

        Option = 1;
        OptionLength = 1;
        LxtCheckErrno(setsockopt(Socket, OptionLevel, OptionName, &Option, OptionLength));

        LxtCheckErrno(getsockopt(Socket, OptionLevel, OptionName, &Option, &OptionLength));

        LxtCheckEqual(Option, 1, "%d");

        //
        // Reset the option value to 0.
        //

        Option = 0;
        OptionLength = sizeof(Option);
        LxtCheckErrno(setsockopt(Socket, OptionLevel, OptionName, &Option, OptionLength));

        //
        // Option size of 2.
        //

        Option = 1;
        OptionLength = 2;
        LxtCheckErrno(setsockopt(Socket, OptionLevel, OptionName, &Option, OptionLength));

        LxtCheckErrno(getsockopt(Socket, OptionLevel, OptionName, &Option, &OptionLength));

        LxtCheckEqual(Option, 1, "%d");

        //
        // Reset the option value to 0.
        //

        Option = 0;
        OptionLength = sizeof(Option);
        LxtCheckErrno(setsockopt(Socket, OptionLevel, OptionName, &Option, OptionLength));

        //
        // Use option size of 3.
        //

        Option = 1;
        OptionLength = 3;
        LxtCheckErrno(setsockopt(Socket, OptionLevel, OptionName, &Option, OptionLength));

        LxtCheckErrno(getsockopt(Socket, OptionLevel, OptionName, &Option, &OptionLength));

        LxtCheckEqual(Option, 1, "%d");
    }

    //
    // Verify that anything above 4 bytes is ignored truncated.
    //

    OptionLong = 0x200000000;
    OptionLength = sizeof(OptionLong);
    LxtCheckErrno(setsockopt(Socket, OptionLevel, OptionName, &OptionLong, OptionLength));

    OptionLength = sizeof(Option);
    LxtCheckErrno(getsockopt(Socket, OptionLevel, OptionName, &Option, &OptionLength));

    LxtCheckEqual(Option, 0, "%d");

ErrorExit:
    return Result;
}

char* SocketGetTypeAsString(int Type)

/*++

Routine Description:

    This routine returns the string equivalent for the give socket type.

Arguments:

    Type - Supplies the socket type.

Return Value:

    Returns the string equivalent for the type; NULL otherwise.

--*/
{

    switch (Type)
    {
    case SOCK_STREAM:
        return LXT_SOCKET_STREAM_STRING;

    case SOCK_DGRAM:
        return LXT_SOCKET_DGRAM_STRING;

    case SOCK_RAW:
        return LXT_SOCKET_RAW_STRING;

    case SOCK_SEQPACKET:
        return LXT_SOCKET_SEQPACKET_STRING;

    case SOCK_PACKET:
        return LXT_SOCKET_PACKET_STRING;

    default:
        return NULL;
    }
}

int SocketStreamClientMsgWaitAll(int ConnectedSocket)

/*++

Routine Description:

    This is a client helper routine testing MSG_WAITALL flag recv syscall.

Arguments:

    ConnectedSocket - Supplies a socket fd.

Return Value:

    0 on success, -1 on failure.

--*/

{
    int FullMessageSize;
    char* ReceiveBuffer;
    int Result = LXT_RESULT_FAILURE;
    char* SendBuffer;
    int Size;

    SendBuffer = LXT_SOCKET_DEFAULT_SEND_STRING;
    FullMessageSize = 2 * strlen(SendBuffer);
    ReceiveBuffer = malloc(FullMessageSize);
    if (ReceiveBuffer == NULL)
    {
        goto ErrorExit;
    }

    LxtLogInfo("Client: 1. send");
    LxtCheckErrno(Size = send(ConnectedSocket, SendBuffer, strlen(SendBuffer), 0));

    //
    // Sleep long enough that the second send won't be concatenated by WSK to
    // test MSW_WAITALL code path, if the socket is inet socket.
    //

    sleep(1);
    LxtLogInfo("Client: 2. delayed send");
    LxtCheckErrno(Size = send(ConnectedSocket, SendBuffer, strlen(SendBuffer), 0));

    memset(ReceiveBuffer, 0, FullMessageSize);
    LxtLogInfo("Client: recv(MSG_WAITALL)");
    LxtCheckErrno(Size = recv(ConnectedSocket, ReceiveBuffer, FullMessageSize, MSG_WAITALL));

    LxtCheckMemoryEqual(SendBuffer, ReceiveBuffer, FullMessageSize / 2);
    LxtCheckMemoryEqual(SendBuffer, ReceiveBuffer + FullMessageSize / 2, sizeof(SendBuffer));

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (ReceiveBuffer != NULL)
    {
        free(ReceiveBuffer);
    }

    return Result;
}

int SocketStreamServerMsgWaitAll(int AcceptedSocket)

/*++

Routine Description:

    This is a server helper routine testing MSG_WAITALL flag recv syscall.

Arguments:

    AcceptedSocket - Supplies a socket fd.

Return Value:

    0 on success, -1 on failure.

--*/

{
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

    LxtLogInfo("Server: recv(MSG_WAITALL)");
    memset(ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    LxtCheckErrno(Size = recv(AcceptedSocket, ReceiveBuffer, FullMessageSize, MSG_WAITALL));

    LxtLogInfo("Server: write all back");
    LxtCheckErrno(write(AcceptedSocket, ReceiveBuffer, Size));
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (ReceiveBuffer != NULL)
    {
        free(ReceiveBuffer);
    }

    return Result;
}
