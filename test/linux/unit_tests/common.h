/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    common.h

Abstract:

    Common socket definitions and helper routines.

--*/

#ifndef _LXT_SOCKET_COMMON_
#define _LXT_SOCKET_COMMON_

#define LXT_SOCKET_CLIENT_VARIATION_SLEEP_SECONDS 5
#define LXT_SOCKET_DEFAULT_BUFFER_LENGTH 512
#define LXT_SOCKET_DEFAULT_PORT 50001
#define LXT_SOCKET_DEFAULT_PORT_IPV6 50002
#define LXT_SOCKET_DEFAULT_PORT_STRING "50001"
#define LXT_SOCKET_DEFAULT_PORT_IPV6_STRING "50002"
#define LXT_SOCKET_VARIATION_TIMEOUT (5 * 1000)
#define LXT_SOCKET_DEFAULT_BACKLOG 32
#define LXT_SOCKET_STREAM_STRING "SOCK_STREAM"
#define LXT_SOCKET_DGRAM_STRING "SOCK_DGRAM"
#define LXT_SOCKET_RAW_STRING "SOCK_RAW"
#define LXT_SOCKET_SEQPACKET_STRING "SOCK_SEQPACKET"
#define LXT_SOCKET_PACKET_STRING "SOCK_PACKET"
#define LXT_SOCKET_AF_INET_STRING "AF_INET"
#define LXT_SOCKET_AF_INET6_STRING "AF_INET6"

#define LxtCheckBytesSendRecv(_requested, _actual) \
    { \
        if ((_requested) != (_actual)) \
        { \
            LxtLogError( \
                "Bytes requested in send/recv do not match actual. " \
                "Requested: %d, Actual:%d.", \
                (_requested), \
                (_actual)); \
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
    }

#define LxtCheckPoll(_numfdsExpected, _numfdsActual, _reventExpected, _reventActual) \
    { \
        if ((_numfdsExpected) != (_numfdsActual)) \
        { \
            LxtLogError("poll returned unexpected value, expecting %d, actual: %d. revents: 0x%x", (_numfdsExpected), (_numfdsActual), (_reventActual)); \
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
\
        if ((_reventExpected) != (_reventActual)) \
        { \
            LxtLogError( \
                "expected epoll events do not match actual. " \
                "Expected: 0x%x, Actual: 0x%x", \
                (_reventExpected), \
                (_reventActual)); \
            Result = LXT_RESULT_FAILURE; \
            goto ErrorExit; \
        } \
    }

//
// This macro will send data from a socket to its connected peer.
//

#define LxtCheckSend(_clientsocket, _sendbuffer, _numbytes, _clientname) \
    { \
        LxtLogInfo("[%s]Sending data to server", (_clientname)); \
        LxtCheckErrno((BytesSent = send((_clientsocket), (_sendbuffer), _numbytes, 0))); \
        LxtCheckBytesSendRecv((ssize_t)_numbytes, BytesSent); \
    }

#define LxtCheckRecv(_servername, _clientname, _recvbuffer, _sendbuffer, _serversocket) \
    { \
        ExpectedBytes = strlen(_sendbuffer); \
        LxtCheckErrno((BytesReceived = recvfrom((_serversocket), (_recvbuffer), sizeof(_recvbuffer), 0, NULL, NULL))); \
        LxtCheckBytesSendRecv(ExpectedBytes, BytesReceived); \
        LxtLogInfo("[%s]Data received from %s", (_servername), (_clientname)); \
    }

//
// TODO: switch over to an array of strings to send / receive.
//

#define LXT_SOCKET_DEFAULT_SEND_STRING "test socket test string\n"

#define LXT_SOCKET_SERVER_MAX_BACKLOG_NUM 5

#define LxtCheckEpoll(_fd, _event, _timeout) \
    { \
        LxtCheckErrno(LxtSocketEpoll((_fd), (_event), (_timeout))); \
    }

#define LxtCheckAncillaryCredentials(_cmsg, _pid, _uid, _gid) \
    { \
        struct ucred* Credentials; \
        LxtCheckEqual((_cmsg)->cmsg_level, SOL_SOCKET, "%d"); \
        LxtCheckEqual((_cmsg)->cmsg_type, SCM_CREDENTIALS, "%d"); \
        LxtCheckEqual((_cmsg)->cmsg_len, CMSG_LEN(sizeof(struct ucred)), "%d"); \
        Credentials = (struct ucred*)CMSG_DATA(_cmsg); \
        LxtCheckEqual(Credentials->pid, (_pid), "%d"); \
        LxtCheckEqual(Credentials->uid, (_uid), "%d"); \
        LxtCheckEqual(Credentials->gid, (_gid), "%d"); \
    }

#define LxtSocketGetDomainAsString(_domain) (((_domain) == AF_INET) ? LXT_SOCKET_AF_INET_STRING : LXT_SOCKET_AF_INET6_STRING)

//
// Private macro to get the next message header which returns the first
// control message when the control message pointer is NULL. glibc's
// 'CMSG_NXTHDR' does not handle that case.
//

#define MY_CMSG_NXTHDR(_msghdr, _pcmsg) (((_pcmsg) == NULL) ? CMSG_FIRSTHDR(_msghdr) : CMSG_NXTHDR(_msghdr, _pcmsg))

int LxtSocketEpoll(int Descriptor, int Event, int Timeout);

void* SocketBlockedReaderThread(void* Arg);

void* SocketBlockedReaderZeroBufferThread(void* Arg);

void* SocketBlockedWriterThread(void* Arg);

struct cmsghdr* SocketGetControlMessage(struct msghdr* MessageHeader, struct cmsghdr* StartControlMessage, int Level, int Type);

int SocketGetSetBooleanSocketOption(int Socket, int OptionLevel, int OptionName, bool SmallerSizeAllowed);

char* SocketGetTypeAsString(int Type);

int SocketStreamClientMsgWaitAll(int ConnectedSocket);

int SocketStreamServerMsgWaitAll(int AcceptedSocket);

#endif // _LXT_SOCKET_COMMON_