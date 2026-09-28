/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    netlink.c

Abstract:

    Linux socket test for the AF_NETLINK family.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>
#include <linux/icmp.h>
#include <netinet/icmp6.h>
#include <net/if.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/prctl.h>
#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include "common.h"

#define LXT_NAME "Netlink"

#define ATTRIBUTE_DUMP_BUFFER_SIZE 60

#define min(a, b) (((a) < (b)) ? (a) : (b))

#define SOCKET_LOOPBACK_IF_NAME "lo"

#define NLMSG_TAIL(nmsg) ((struct rtattr*)(((void*)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

const int MessageTypes[] = {
    RTM_DELADDR, RTM_DELLINK, RTM_DELROUTE, RTM_GETADDR, RTM_GETLINK, RTM_GETNSID, RTM_GETROUTE, RTM_NEWADDR, RTM_NEWLINK, RTM_NEWROUTE, RTM_SETLINK};

const int SupportedFamily[] = {NETLINK_ROUTE};
const int SupportedType[] = {SOCK_DGRAM, SOCK_RAW};

typedef struct _NetlinkRecvmmsgBlockedReaderParams
{
    int Socket;
    int Option;
} NetlinkRecvmmsgBlockedReaderParams;

typedef struct _BindChildThreadReturn
{
    pid_t nl_pid;
} BindChildThreadReturn;

LXT_VARIATION_HANDLER SocketNetlinkOpenClose;
LXT_VARIATION_HANDLER SocketNetlinkBasic;
LXT_VARIATION_HANDLER SocketNetlinkBind;
LXT_VARIATION_HANDLER SocketNetlinkBindThread;
LXT_VARIATION_HANDLER SocketNetlinkSendReceive;
LXT_VARIATION_HANDLER SocketNetlinkSendReceiveOverflow;
LXT_VARIATION_HANDLER SocketNetlinkBlockedReader;
LXT_VARIATION_HANDLER SocketNetlinkEpoll;
LXT_VARIATION_HANDLER SocketNetlinkRecvmmsg;
LXT_VARIATION_HANDLER SocketNetlinkSendBadMessage;
LXT_VARIATION_HANDLER SocketNetlinkRouteGetAddr;
LXT_VARIATION_HANDLER SocketNetlinkRouteGetLink;
LXT_VARIATION_HANDLER SocketNetlinkRouteGetRouteBestRoute;
LXT_VARIATION_HANDLER SocketNetlinkRouteGetRouteDump;
LXT_VARIATION_HANDLER SocketNetlinkRouteNewDelAddress;
LXT_VARIATION_HANDLER SocketNetlinkRouteNewDelRoute;
LXT_VARIATION_HANDLER SocketNetlinkSoPasscred;

void* SocketNetlinkBindThreadChild(void* Arg);

bool SocketNetlinkIsIpv6Configured();

void* SocketNetlinkRecvmmsgBlockedReaderThread(void* Arg);

void SocketNetlinkRouteDumpAttributeData(char* Buffer, int BufferSize, struct rtattr* Attribute);

int SocketNetlinkRouteGetLinkCheckResponse(int Socket, int OnlyOneLoopback);

int SocketNetlinkRouteGetRouteBestRouteCheckResponse(int Socket, int CheckGateway);

int SocketNetlinkGetLoopbackIndex();

int SocketNetlinkCheckResponseError(void* ReceiveBuffer, int ReceiveResult, int Error);

int SocketNetlinkRouteAddAttribute(struct nlmsghdr* Msghdr, int MessageSize, int AttributeType, void* AttributeData, int AttributeSize);

void SocketNetlinkRouteAddAddressAttributes(struct nlmsghdr* Msghdr, int MessageSize, struct in_addr* AddressIpv4);

void SocketNetlinkRouteAddRouteAttributes(
    struct nlmsghdr* Msghdr, int MessageSize, struct in_addr* DestinationIpv4, struct in_addr* GatewayIpv4, int InterfaceIndex);

int SocketNetlinkSendAndWaitForExpectedError(int Socket, void* Request, int RequestSize, int ExpectedError);

int SocketNetlinkSetAndVerifySocketOptionTimeout(int Socket, int SocketOption, int Usec);

static const LXT_VARIATION g_LxtVariations[] = {
    {"Open, close", SocketNetlinkOpenClose},
    {"Basic netlink operations", SocketNetlinkBasic},
    {"bind, getsockname (with fork)", SocketNetlinkBind},
    {"bind, getsockname (with pthread_create)", SocketNetlinkBindThread},
    {"Send and receive basic (sending to kernel)", SocketNetlinkSendReceive},
    {"Send and receive where the receive buffer overflows (sending to kernel)", SocketNetlinkSendReceiveOverflow},
    {"Blocked reader thread", SocketNetlinkBlockedReader},
    {"epoll", SocketNetlinkEpoll},
    {"Recvmmsg syscall", SocketNetlinkRecvmmsg},
    {"Sending bad Netlink messages", SocketNetlinkSendBadMessage},
    {"NETLINK_ROUTE RTM_GETADDR message", SocketNetlinkRouteGetAddr},
    {"NETLINK_ROUTE RTM_GETLINK message", SocketNetlinkRouteGetLink},
    {"NETLINK_ROUTE RTM_GETROUTE message - get best route", SocketNetlinkRouteGetRouteBestRoute},
    {"NETLINK_ROUTE RTM_GETROUTE message - dump all routing entries", SocketNetlinkRouteGetRouteDump},
    {"NETLINK_ROUTE RTM_NEWADDR and RTM_DELADDR message", SocketNetlinkRouteNewDelAddress},
    {"NETLINK_ROUTE RTM_NEWROUTE and RTM_DELROUTE message", SocketNetlinkRouteNewDelRoute},
    {"SO_PASSCRED", SocketNetlinkSoPasscred},
};

//
// Function definitions.
//

int NetlinkTestEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine main entry point for the test for get*id,set*id system call.

Arguments:

    Argc - Supplies the number of command line arguments.

    Argv - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    LXT_ARGS Args;
    int Result;

    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));

    //
    // Run test cases.
    //

    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:
    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

bool SocketNetlinkIsIpv6Configured()

/*++

Routine Description:

    This routine checks if an IPv6 address has been configured.

Arguments:

    None.

Return Value:

    Returns true if an IPv6 address is available.

--*/

{

    socklen_t AddressLength;
    struct rtattr* Attribute;
    char AttributeDump[ATTRIBUTE_DUMP_BUFFER_SIZE];
    struct sockaddr_nl BindAddress;
    int FoundDone;
    struct nlmsghdr* Header;
    struct ifaddrmsg* IfAddrMsg;
    bool IpV6AddressValid;
    int LoopbackIndex;
    char ReceiveBuffer[5000];
    int ReceiveResult;
    int RemainingLength;
    int Result;
    int Socket;

    struct
    {
        struct nlmsghdr nlh;
        struct ifinfomsg ifm;
        struct rtattr ext_req __attribute__((aligned(NLMSG_ALIGNTO)));
        __u32 ext_filter_mask;
    } Request;

    IpV6AddressValid = false;

    LxtCheckErrnoZeroSuccess(SocketNetlinkGetLoopbackIndex(&LoopbackIndex));

    //
    // Create and bind socket. Create a RTM_GETADDR request.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = sizeof(Request);
    Request.nlh.nlmsg_type = RTM_GETADDR;
    Request.nlh.nlmsg_seq = 0x4563;
    Request.ifm.ifi_family = AF_NETLINK;
    Request.ext_req.rta_type = IFLA_EXT_MASK;
    Request.ext_req.rta_len = RTA_LENGTH(sizeof(__u32));
    Request.ext_filter_mask = RTEXT_FILTER_VF;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    FoundDone = 0;
    for (;;)
    {
        LxtCheckErrno(ReceiveResult = recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0, NULL, 0));

        Header = (struct nlmsghdr*)ReceiveBuffer;
        while (!IpV6AddressValid && NLMSG_OK(Header, ReceiveResult))
        {
            if (Header->nlmsg_type == NLMSG_DONE)
            {
                FoundDone = 1;
                break;
            }

            IfAddrMsg = (struct ifaddrmsg*)NLMSG_DATA(Header);
            if ((IfAddrMsg->ifa_index != LoopbackIndex) && (IfAddrMsg->ifa_family == AF_INET6))
            {

                Attribute = (struct rtattr*)((char*)IfAddrMsg + sizeof(struct ifaddrmsg));

                RemainingLength = Header->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifaddrmsg));

                while (RTA_OK(Attribute, RemainingLength))
                {
                    SocketNetlinkRouteDumpAttributeData(AttributeDump, ATTRIBUTE_DUMP_BUFFER_SIZE, Attribute);

                    //
                    // If the address is IPv6 and does not start with the link
                    // local prefix, assume it is a valid address.
                    //

                    if ((Attribute->rta_type == IFA_ADDRESS) && strncmp(AttributeDump, "fe 80", 5) != 0)
                    {

                        LxtLogInfo("IpV6 address found: %s", AttributeDump);
                        IpV6AddressValid = true;
                        break;
                    }

                    Attribute = RTA_NEXT(Attribute, RemainingLength);
                }
            }

            Header = NLMSG_NEXT(Header, ReceiveResult);
        }

        if (IpV6AddressValid || (FoundDone == 1))
        {
            break;
        }
    }

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return IpV6AddressValid;
}

int SocketNetlinkOpenClose(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the socket() API.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int FamilyIterator;
    int Result;
    int Socket;
    int TypeIterator;

    Result = LXT_RESULT_FAILURE;
    Socket = -1;

    for (TypeIterator = 0; TypeIterator < sizeof(SupportedType) / sizeof(int); TypeIterator++)
    {

        for (FamilyIterator = 0; FamilyIterator < sizeof(SupportedFamily) / sizeof(int); FamilyIterator++)
        {

            LxtLogInfo("testing type: %d, netlink family: %d", SupportedType[TypeIterator], SupportedFamily[FamilyIterator]);

            //
            // Success cases.
            //

            LxtCheckErrno((Socket = socket(AF_NETLINK, SupportedType[TypeIterator], SupportedFamily[FamilyIterator])));

            LxtClose(Socket);

            //
            // Test case: Overloading 'type' field for netlink sockets.
            //

            LxtCheckErrno((Socket = socket(AF_NETLINK, SupportedType[TypeIterator] | O_NONBLOCK, SupportedFamily[FamilyIterator])));

            LxtClose(Socket);
            LxtCheckErrno((Socket = socket(AF_NETLINK, SupportedType[TypeIterator] | O_CLOEXEC, SupportedFamily[FamilyIterator])));

            LxtClose(Socket);
            LxtCheckErrno((Socket = socket(AF_NETLINK, SupportedType[TypeIterator] | O_NONBLOCK | O_CLOEXEC, SupportedFamily[FamilyIterator])));

            LxtClose(Socket);
        }
    }

    //
    // Test case: Failure cases for socket(), unsupported type, family.
    //

    LxtCheckErrnoFailure(socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE + 100), EPROTONOSUPPORT);

    LxtCheckErrnoFailure(socket(AF_NETLINK, SOCK_STREAM, NETLINK_ROUTE), ESOCKTNOSUPPORT);

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SocketNetlinkBasic(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests basic operations on netlink sockets.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct sockaddr_nl BindAddress;
    int BufferSize;
    int FamilyIterator;
    socklen_t OptionLength;
    int OptionInt;
    char ReceiveBuffer[500];
    int Result;
    int Socket;
    int SocketError;
    struct timeval Timeout;
    int TypeIterator;

    Result = LXT_RESULT_FAILURE;
    Socket = -1;

    for (TypeIterator = 0; TypeIterator < sizeof(SupportedType) / sizeof(int); TypeIterator++)
    {

        for (FamilyIterator = 0; FamilyIterator < sizeof(SupportedFamily) / sizeof(int); FamilyIterator++)
        {

            LxtCheckErrno(Socket = socket(AF_NETLINK, SupportedType[TypeIterator], SupportedFamily[FamilyIterator]));

            memset(&BindAddress, 0, sizeof(BindAddress));
            BindAddress.nl_family = AF_NETLINK;
            AddressLength = sizeof(BindAddress);
            LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));
            OptionLength = sizeof(BufferSize);
            BufferSize = 2345;
            LxtCheckErrno(setsockopt(Socket, SOL_SOCKET, SO_SNDBUF, &BufferSize, OptionLength));

            LxtCheckErrno(getsockopt(Socket, SOL_SOCKET, SO_SNDBUF, &BufferSize, &OptionLength));

            LxtCheckEqual(OptionLength, sizeof(BufferSize), "%d");
            LxtCheckEqual(BufferSize, 2345 * 2, "%d");

            OptionLength = sizeof(BufferSize);
            BufferSize = 6345;
            LxtCheckErrno(setsockopt(Socket, SOL_SOCKET, SO_RCVBUF, &BufferSize, OptionLength));

            LxtCheckErrno(getsockopt(Socket, SOL_SOCKET, SO_RCVBUF, &BufferSize, &OptionLength));

            LxtCheckEqual(OptionLength, sizeof(BufferSize), "%d");
            LxtCheckEqual(BufferSize, 6345 * 2, "%d");

            //
            // Set a timeout value of 8 milliseconds for the send.
            //

            LxtCheckResult(SocketNetlinkSetAndVerifySocketOptionTimeout(Socket, SO_SNDTIMEO, 8000));

            //
            // Set a timeout value of 8 milliseconds for the receive.
            //

            LxtCheckResult(SocketNetlinkSetAndVerifySocketOptionTimeout(Socket, SO_RCVTIMEO, 8000));

            //
            // Check that the blocking read will timeout with EAGAIN
            // after 8 milliseconds.
            //

            LxtCheckErrnoFailure(read(Socket, ReceiveBuffer, sizeof(ReceiveBuffer)), EAGAIN);

            //
            // Check that the value of SO_ERROR is 0 (no error).
            //

            SocketError = 1234;
            OptionLength = sizeof(SocketError);
            LxtCheckErrno(getsockopt(Socket, SOL_SOCKET, SO_ERROR, &SocketError, &OptionLength));

            LxtCheckEqual(OptionLength, sizeof(SocketError), "%d");
            LxtCheckEqual(SocketError, 0, "%d");

            OptionLength = sizeof(OptionInt);
            LxtCheckErrno(getsockopt(Socket, SOL_SOCKET, SO_TYPE, &OptionInt, &OptionLength));

            LxtCheckEqual(OptionLength, sizeof(OptionInt), "%d");
            LxtCheckEqual(OptionInt, SupportedType[TypeIterator], "%d");

            OptionLength = sizeof(OptionInt);
            LxtCheckErrno(getsockopt(Socket, SOL_SOCKET, SO_PROTOCOL, &OptionInt, &OptionLength));

            LxtCheckEqual(OptionLength, sizeof(OptionInt), "%d");
            LxtCheckEqual(OptionInt, SupportedFamily[FamilyIterator], "%d");

            OptionLength = sizeof(OptionInt);
            LxtCheckErrno(getsockopt(Socket, SOL_SOCKET, SO_DOMAIN, &OptionInt, &OptionLength));

            LxtCheckEqual(OptionLength, sizeof(OptionInt), "%d");
            LxtCheckEqual(OptionInt, AF_NETLINK, "%d");
            LxtClose(Socket);
        }
    }

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SocketNetlinkBind(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the bind() API.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char AddressBuffer[150];
    socklen_t AddressLength;
    int ArrayIterator;
    int ArrayIterator2;
    const int ArraySize = 10;
    struct sockaddr_nl BindAddress;
    struct sockaddr_nl* BindAddressP;
    int ChildPid;
    char DataBuffer[10];
    int Family;
    int FamilyIterator;
    int Result;
    uint32_t SavedPid;
    int Socket;
    int Socket2;
    int Socket3;
    int Socket4;
    int SocketA[ArraySize];
    int SocketChildA[ArraySize];
    int SocketChildPid[ArraySize];
    int SocketPid[ArraySize];
    int Type;
    int TypeIterator;

    Result = LXT_RESULT_FAILURE;
    Socket = -1;
    Socket2 = -1;
    Socket3 = -1;
    Socket4 = -1;
    ChildPid = -1;
    for (TypeIterator = 0; TypeIterator < sizeof(SupportedType) / sizeof(int); TypeIterator++)
    {

        Type = SupportedType[TypeIterator];
        for (FamilyIterator = 0; FamilyIterator < sizeof(SupportedFamily) / sizeof(int); FamilyIterator++)
        {

            //
            // First thing; initialize the various array.
            //

            for (ArrayIterator = 0; ArrayIterator < ArraySize; ArrayIterator++)
            {
                SocketA[ArrayIterator] = 0;
                SocketPid[ArrayIterator] = 0;
                SocketChildPid[ArrayIterator] = 0;
            }

            Family = SupportedFamily[FamilyIterator];
            LxtLogInfo("testing type: %d, netlink family: %d", SupportedType[TypeIterator], SupportedFamily[FamilyIterator]);

            //
            // Test case: bind with invalid family value in the address.
            //

            LxtCheckErrno(Socket = socket(AF_NETLINK, Type, Family));
            memset(&BindAddress, 0, sizeof(BindAddress));
            BindAddress.nl_family = AF_INET;
            AddressLength = sizeof(BindAddress);
            LxtCheckErrnoFailure(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength), EINVAL);

            LxtClose(Socket);

            //
            // Test case: bind with invalid address length.
            //

            LxtCheckErrno(Socket = socket(AF_NETLINK, Type, Family));
            memset(&BindAddress, 0, sizeof(BindAddress));
            BindAddress.nl_family = AF_NETLINK;
            AddressLength = sizeof(BindAddress.nl_family);
            LxtCheckErrnoFailure(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength), EINVAL);

            LxtClose(Socket);

            //
            // Test case: address length > sizeof(sockaddr_nl) is also invalid.
            //

            LxtCheckErrno(Socket = socket(AF_NETLINK, Type, Family));
            BindAddressP = (struct sockaddr_nl*)AddressBuffer;
            memset(AddressBuffer, 0, sizeof(AddressBuffer));
            BindAddressP->nl_family = AF_NETLINK;
            AddressLength = sizeof(AddressBuffer);
            LxtCheckErrnoFailure(bind(Socket, (struct sockaddr*)BindAddressP, AddressLength), EINVAL);

            LxtClose(Socket);

            //
            // Test case: getsockname on unbound socket returns nl_pid = 0.
            //

            LxtCheckErrno(Socket = socket(AF_NETLINK, Type, Family));
            memset(&BindAddress, 0, sizeof(BindAddress));
            AddressLength = sizeof(BindAddress);
            LxtCheckErrno(getsockname(Socket, (struct sockaddr*)&BindAddress, &AddressLength));

            LxtCheckEqual(BindAddress.nl_pid, 0, "%d");
            LxtCheckEqual(AddressLength, sizeof(BindAddress), "%d");
            LxtClose(Socket);

            //
            // Test case: calling sendto() on unbound socket automatically
            //            binds the socket (even if the sendto() fails).
            //

            LxtCheckErrno(Socket = socket(AF_NETLINK, Type, Family));
            memset(DataBuffer, 0, sizeof(DataBuffer));
            memset(&BindAddress, 0, sizeof(BindAddress));
            BindAddress.nl_family = AF_NETLINK;
            AddressLength = sizeof(BindAddress);
            sendto(Socket, DataBuffer, sizeof(DataBuffer), 0, (struct sockaddr*)&BindAddress, AddressLength);

            memset(&BindAddress, 0, sizeof(BindAddress));
            LxtCheckErrno(getsockname(Socket, (struct sockaddr*)&BindAddress, &AddressLength));

            LxtCheckEqual(BindAddress.nl_pid, getpid(), "%d");
            LxtCheckEqual(AddressLength, sizeof(BindAddress), "%d");
            LxtClose(Socket);

            //
            // Test case: Bind with nl_pid = 0, kernel should assign a unique
            //            ID. The first netlink socket of the process is
            //            assigned the process ID as the nl_pid. It also confirms
            //            the pad value is ignored.
            //

            LxtCheckErrno(Socket = socket(AF_NETLINK, Type, Family));
            memset(&BindAddress, 0, sizeof(BindAddress));
            BindAddress.nl_family = AF_NETLINK;
            BindAddress.nl_pad = 1;
            AddressLength = sizeof(BindAddress);
            LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

            memset(&BindAddress, 0, sizeof(BindAddress));
            LxtCheckErrno(getsockname(Socket, (struct sockaddr*)&BindAddress, &AddressLength));

            LxtCheckEqual(BindAddress.nl_pid, getpid(), "%d");
            LxtCheckEqual(AddressLength, sizeof(BindAddress), "%d");

            //
            // Validate that a socket that is already bound, cannot be bound again.
            //

            memset(&BindAddress, 0, sizeof(BindAddress));
            BindAddress.nl_family = AF_NETLINK;
            AddressLength = sizeof(BindAddress);
            LxtCheckErrnoFailure(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength), EINVAL);

            //
            // Test case: The user specifies a negative nl_pid (which is what the
            //            kernel assigns to the non-first sockets of the process).
            //            Test that the kernel will skip any negative nl_pid's that
            //            the user already specified. For example, Socket2 gets
            //            an auto-assigned nl_pid of -5. Socket3 is bound with
            //            the user specifying nl_pid of -4. Socket4 gets an
            //            auto-assigned nl_pid of -3, since -4 was already taken
            //            by the user.
            //

            LxtCheckErrno(Socket2 = socket(AF_NETLINK, Type, Family));
            memset(&BindAddress, 0, sizeof(BindAddress));
            BindAddress.nl_family = AF_NETLINK;
            AddressLength = sizeof(BindAddress);
            LxtCheckErrno(bind(Socket2, (struct sockaddr*)&BindAddress, AddressLength));

            memset(&BindAddress, 0, sizeof(BindAddress));
            LxtCheckErrno(getsockname(Socket2, (struct sockaddr*)&BindAddress, &AddressLength));

            LxtCheckTrue((int)BindAddress.nl_pid < 0);
            LxtCheckEqual(AddressLength, sizeof(BindAddress), "%d");

            LxtCheckErrno(Socket3 = socket(AF_NETLINK, Type, Family));
            BindAddress.nl_pid += 1;
            AddressLength = sizeof(BindAddress);
            LxtCheckErrno(bind(Socket3, (struct sockaddr*)&BindAddress, AddressLength));

            memset(&BindAddress, 0, sizeof(BindAddress));
            LxtCheckErrno(getsockname(Socket3, (struct sockaddr*)&BindAddress, &AddressLength));

            LxtCheckTrue((int)BindAddress.nl_pid < 0);
            LxtCheckEqual(AddressLength, sizeof(BindAddress), "%d");

            LxtCheckErrno(Socket4 = socket(AF_NETLINK, Type, Family));
            memset(&BindAddress, 0, sizeof(BindAddress));
            BindAddress.nl_family = AF_NETLINK;
            AddressLength = sizeof(BindAddress);
            LxtCheckErrno(bind(Socket4, (struct sockaddr*)&BindAddress, AddressLength));

            memset(&BindAddress, 0, sizeof(BindAddress));
            LxtCheckErrno(getsockname(Socket4, (struct sockaddr*)&BindAddress, &AddressLength));

            LxtCheckTrue((int)BindAddress.nl_pid < 0);
            LxtCheckEqual(AddressLength, sizeof(BindAddress), "%d");
            LxtClose(Socket3);
            LxtClose(Socket4);

            //
            // Test case: The kernel assigned a negative custom PID for Socket2.
            //            Save Socket2's custom PID, close Socket2, then bind a
            //            new socket with the saved PID, and verify success.
            //

            AddressLength = sizeof(BindAddress);
            memset(&BindAddress, 0, sizeof(BindAddress));
            LxtCheckErrno(getsockname(Socket2, (struct sockaddr*)&BindAddress, &AddressLength));

            LxtCheckTrue((int)BindAddress.nl_pid < 0);
            LxtCheckEqual(AddressLength, sizeof(BindAddress), "%d");
            SavedPid = BindAddress.nl_pid;
            LxtClose(Socket2);
            LxtCheckErrno(Socket2 = socket(AF_NETLINK, Type, Family));
            AddressLength = sizeof(BindAddress);
            LxtCheckErrno(bind(Socket2, (struct sockaddr*)&BindAddress, AddressLength));

            memset(&BindAddress, 0, sizeof(BindAddress));
            LxtCheckErrno(getsockname(Socket2, (struct sockaddr*)&BindAddress, &AddressLength));

            LxtCheckEqual((int)BindAddress.nl_pid, SavedPid, "%d");
            LxtCheckEqual(AddressLength, sizeof(BindAddress), "%d");
            LxtClose(Socket2);

            //
            // Test case: close the first socket of the process and open a new
            //            one. Being the first netlink socket of the process,
            //            it should get the process ID as the 'nl_pid'.
            //

            LxtClose(Socket);
            LxtCheckErrno(Socket = socket(AF_NETLINK, Type, Family));
            memset(&BindAddress, 0, sizeof(BindAddress));
            BindAddress.nl_family = AF_NETLINK;
            AddressLength = sizeof(BindAddress);
            LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

            memset(&BindAddress, 0, sizeof(BindAddress));
            LxtCheckErrno(getsockname(Socket, (struct sockaddr*)&BindAddress, &AddressLength));

            LxtCheckEqual(BindAddress.nl_pid, getpid(), "%d");
            LxtCheckEqual(AddressLength, sizeof(BindAddress), "%d");

            //
            // Test case: Kernel should assign a negative unique ID to any subsequent
            //            (after the first one) netlink socket that the process
            //            subsequently creates.
            //

            for (ArrayIterator = 0; ArrayIterator < ArraySize; ArrayIterator++)
            {
                LxtCheckErrno(SocketA[ArrayIterator] = socket(AF_NETLINK, Type, Family));

                memset(&BindAddress, 0, sizeof(BindAddress));
                BindAddress.nl_family = AF_NETLINK;
                AddressLength = sizeof(BindAddress);
                LxtCheckErrno(bind(SocketA[ArrayIterator], (struct sockaddr*)&BindAddress, AddressLength));

                memset(&BindAddress, 0, sizeof(BindAddress));
                LxtCheckErrno(getsockname(SocketA[ArrayIterator], (struct sockaddr*)&BindAddress, &AddressLength));

                SocketPid[ArrayIterator] = BindAddress.nl_pid;
                LxtCheckTrue(SocketPid[ArrayIterator] < 0);
            }

            //
            // Validate that every socket PID is unique.
            //

            for (ArrayIterator = 0; ArrayIterator < ArraySize; ArrayIterator++)
            {
                LxtLogInfo("parent, socket: %d, pid: %d", ArrayIterator + 1, SocketPid[ArrayIterator]);

                for (ArrayIterator2 = ArrayIterator + 1; ArrayIterator2 < ArraySize; ArrayIterator2++)
                {

                    LxtCheckNotEqual(SocketPid[ArrayIterator], SocketPid[ArrayIterator2], "%d");
                }
            }

            //
            // Test case: validate that the child process gets its own set of ID's.
            //

            ChildPid = fork();
            if (ChildPid == 0)
            {
                LxtLogInfo("child, pid: %d", getpid());
                LxtClose(Socket);
                LxtCheckErrno(Socket = socket(AF_NETLINK, Type, Family));
                memset(&BindAddress, 0, sizeof(BindAddress));
                BindAddress.nl_family = AF_NETLINK;
                AddressLength = sizeof(BindAddress);
                LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

                memset(&BindAddress, 0, sizeof(BindAddress));
                LxtCheckErrno(getsockname(Socket, (struct sockaddr*)&BindAddress, &AddressLength));

                LxtCheckEqual(BindAddress.nl_pid, getpid(), "%d");
                for (ArrayIterator = 0; ArrayIterator < ArraySize; ArrayIterator++)
                {
                    LxtCheckErrno(SocketChildA[ArrayIterator] = socket(AF_NETLINK, Type, Family));

                    memset(&BindAddress, 0, sizeof(BindAddress));
                    BindAddress.nl_family = AF_NETLINK;
                    AddressLength = sizeof(BindAddress);
                    LxtCheckErrno(bind(SocketChildA[ArrayIterator], (struct sockaddr*)&BindAddress, AddressLength));

                    memset(&BindAddress, 0, sizeof(BindAddress));
                    LxtCheckErrno(getsockname(SocketChildA[ArrayIterator], (struct sockaddr*)&BindAddress, &AddressLength));

                    SocketChildPid[ArrayIterator] = BindAddress.nl_pid;
                    LxtCheckTrue(SocketChildPid[ArrayIterator] < 0);
                }

                //
                // Validate that every socket PID is unique, locally as well
                // as globally.
                //

                for (ArrayIterator = 0; ArrayIterator < ArraySize; ArrayIterator++)
                {
                    LxtLogInfo("child, socket: %d, pid: %d", ArrayIterator + 1, SocketChildPid[ArrayIterator]);

                    for (ArrayIterator2 = ArrayIterator + 1; ArrayIterator2 < ArraySize; ArrayIterator2++)
                    {

                        LxtCheckNotEqual(SocketChildPid[ArrayIterator], SocketChildPid[ArrayIterator2], "%d");

                        LxtCheckNotEqual(SocketPid[ArrayIterator], SocketChildPid[ArrayIterator2], "%d");
                    }
                }

                for (ArrayIterator = 0; ArrayIterator < ArraySize; ArrayIterator++)
                {
                    LxtClose(SocketChildA[ArrayIterator]);
                }

                LxtClose(Socket);
                Result = LXT_RESULT_SUCCESS;
                goto ErrorExit;
            }

            LxtCheckResult(LxtWaitPidPoll(ChildPid, LXT_RESULT_SUCCESS));
            for (ArrayIterator = 0; ArrayIterator < ArraySize; ArrayIterator++)
            {
                LxtClose(SocketA[ArrayIterator]);
            }

            //
            // Test case: validate that if the app provides a PID for the socket,
            //            then it should be unique.
            //

            LxtCheckErrno(SocketA[0] = socket(AF_NETLINK, Type, Family));
            memset(&BindAddress, 0, sizeof(BindAddress));
            BindAddress.nl_family = AF_NETLINK;

            //
            // Since the very first socket of this process is still open,
            // getpid() as the socket PID is already in use.
            //

            BindAddress.nl_pid = getpid();
            AddressLength = sizeof(BindAddress);
            LxtCheckErrnoFailure(bind(SocketA[0], (struct sockaddr*)&BindAddress, AddressLength), EADDRINUSE);

            LxtClose(SocketA[0]);
            LxtClose(Socket);

            //
            // Test case: validate that when there are no netlink sockets opened
            //            by the process, the app should be able to provide the
            //            PID of the process as the unique ID for the socket.
            //

            LxtCheckErrno(Socket = socket(AF_NETLINK, Type, Family));
            memset(&BindAddress, 0, sizeof(BindAddress));
            BindAddress.nl_family = AF_NETLINK;
            BindAddress.nl_pid = getpid();
            AddressLength = sizeof(BindAddress);
            LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

            LxtClose(Socket);
        }
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    if (Socket2 > 0)
    {
        close(Socket2);
    }

    if (Socket3 > 0)
    {
        close(Socket3);
    }

    if (Socket4 > 0)
    {
        close(Socket4);
    }

    for (ArrayIterator = 0; ArrayIterator < ArraySize; ArrayIterator++)
    {
        if (SocketA[ArrayIterator] != 0)
        {
            close(SocketA[ArrayIterator]);
        }

        if (SocketChildA[ArrayIterator] != 0)
        {
            close(SocketChildA[ArrayIterator]);
        }
    }

    //
    // If child, exit.
    //

    if (ChildPid == 0)
    {
        _exit(Result);
    }

    return Result;
}

int SocketNetlinkBindThread(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the bind() API with new threads
    created by pthread_create.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    int ArrayIterator;
    struct sockaddr_nl BindAddress;
    pthread_t ChildThread;
    pid_t ChildThreadNetlinkPid;
    BindChildThreadReturn* ChildThreadReturn;
    int Result;
    int Socket;

    Result = LXT_RESULT_FAILURE;
    Socket = -1;
    for (ArrayIterator = 0; ArrayIterator < 5; ArrayIterator++)
    {

        //
        // Open a netlink socket. This is the first netlink socket of the
        // threadgroup, so it should have a nl_pid equal to getpid() (which
        // is the tgid).
        //

        LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
        memset(&BindAddress, 0, sizeof(BindAddress));
        BindAddress.nl_family = AF_NETLINK;
        AddressLength = sizeof(BindAddress);
        LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

        memset(&BindAddress, 0, sizeof(BindAddress));
        LxtCheckErrno(getsockname(Socket, (struct sockaddr*)&BindAddress, &AddressLength));

        LxtLogInfo("nl_pid 1: %d pid: %d", BindAddress.nl_pid, getpid());
        LxtCheckTrue(BindAddress.nl_pid > 0);
        LxtCheckEqual(BindAddress.nl_pid, getpid(), "%d");

        //
        // Create a new thread, which opens and closes a netlink socket in the
        // new thread. This is not the first netlink socket of the threadgroup,
        // so it should have a negative nl_pid.
        //

        LxtCheckErrnoZeroSuccess(pthread_create(&ChildThread, NULL, SocketNetlinkBindThreadChild, NULL));

        LxtCheckErrnoZeroSuccess(pthread_join(ChildThread, (void*)&ChildThreadReturn));
        LxtCheckTrue(ChildThreadReturn != NULL);
        ChildThreadNetlinkPid = ChildThreadReturn->nl_pid;
        free(ChildThreadReturn);
        LxtLogInfo("nl_pid 2: %d pid: %d", ChildThreadNetlinkPid, getpid());
        LxtCheckTrue(ChildThreadNetlinkPid < 0);

        //
        // Close the first netlink socket (leaving no open netlink sockets).
        // Create a new thread, which opens and closes a netlink socket in the
        // new thread. This is the first netlink socket of the threadgroup.
        //

        LxtClose(Socket);
        LxtCheckErrnoZeroSuccess(pthread_create(&ChildThread, NULL, SocketNetlinkBindThreadChild, NULL));

        LxtCheckErrnoZeroSuccess(pthread_join(ChildThread, (void*)&ChildThreadReturn));
        LxtCheckTrue(ChildThreadReturn != NULL);
        ChildThreadNetlinkPid = ChildThreadReturn->nl_pid;
        free(ChildThreadReturn);
        LxtLogInfo("nl_pid 3: %d pid: %d", ChildThreadNetlinkPid, getpid());
        LxtCheckTrue(ChildThreadNetlinkPid > 0);
        LxtCheckEqual(ChildThreadNetlinkPid, getpid(), "%d");
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

void* SocketNetlinkBindThreadChild(void* Arg)

/*++

Routine Description:

    This routine runs in a child thread and creates a netlink socket.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    On success, returns a struct containing the nl_pid of the netlink socket
    created by this thread. On failure, returns NULL.

--*/

{

    socklen_t AddressLength;
    struct sockaddr_nl BindAddress;
    int Result;
    int Socket;
    BindChildThreadReturn* ThreadReturn;

    Socket = -1;
    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    memset(&BindAddress, 0, sizeof(BindAddress));
    LxtCheckErrno(getsockname(Socket, (struct sockaddr*)&BindAddress, &AddressLength));

    LxtLogInfo("Child thread: nl_pid: %d, getpid(): %d", BindAddress.nl_pid, getpid());

    LxtClose(Socket);
    ThreadReturn = malloc(sizeof(*ThreadReturn));
    LxtCheckTrue(ThreadReturn != NULL);
    ThreadReturn->nl_pid = BindAddress.nl_pid;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    if (Result < 0)
    {
        return NULL;
    }
    else
    {
        return ThreadReturn;
    }
}

int SocketNetlinkSendReceive(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the send and receive family of APIs.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct sockaddr_nl BindAddress;
    struct
    {
        struct nlmsghdr nlh1;
        char dummy[4];
        struct nlmsghdr nlh2;
    } DoubleRequest;
    struct nlmsgerr* Error;
    int ExpectedReceiveLength;
    struct iovec IoVec[10];
    struct msghdr MessageHeader;
    struct sockaddr_nl ReceiveAddress;
    char ReceiveBuffer[500];
    struct nlmsghdr* ReceiveHeader;
    struct nlmsghdr* ReceiveHeader2;
    int ReceiveResult;
    struct nlmsghdr Request;
    int Result;
    struct sockaddr_nl SendAddress;
    uint32_t Sequence;
    int Socket;

    Result = LXT_RESULT_FAILURE;
    Socket = -1;
    Sequence = 0x7435;

    //
    // Create and bind socket. Create a Netlink request with the ACK flag.
    // Netlink should echo the same request back to us.
    //
    // TODO_LX: Test whether invalid messages should be ACK'd back.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    memset(&Request, 0, sizeof(Request));
    Request.nlmsg_len = NLMSG_LENGTH(0);
    Request.nlmsg_type = NLMSG_NOOP;
    Request.nlmsg_flags = NLM_F_ACK;
    Request.nlmsg_seq = Sequence;

    //
    // Test sendto() with invalid send addresses.
    //

    AddressLength = sizeof(SendAddress);
    memset(&SendAddress, 0, sizeof(SendAddress));
    SendAddress.nl_family = AF_NETLINK;
    SendAddress.nl_pid = 0xFFFF1234;
    LxtCheckErrnoFailure(sendto(Socket, &Request, sizeof(Request), 0, (struct sockaddr*)&SendAddress, AddressLength), ECONNREFUSED);

    memset(&SendAddress, 0, sizeof(SendAddress));
    SendAddress.nl_family = 0xFFFF;
    LxtCheckErrnoFailure(sendto(Socket, &Request, sizeof(Request), 0, (struct sockaddr*)&SendAddress, AddressLength), EINVAL);

    //
    // Test sendto() with 0 nl_pid in send address. This should go to the kernel.
    // This is message 0.
    //

    memset(&SendAddress, 0, sizeof(SendAddress));
    Request.nlmsg_seq = Sequence;
    SendAddress.nl_family = AF_NETLINK;
    SendAddress.nl_pid = 0;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, (struct sockaddr*)&SendAddress, AddressLength));

    //
    // Test sendto() with 0 buffer length. The return value should be 0
    // and no response should be generated.
    //

    LxtCheckErrnoZeroSuccess(sendto(Socket, &Request, 0, 0, (struct sockaddr*)&SendAddress, AddressLength));

    //
    // Test sendto() with NULL send address. This should go to the kernel
    // by default. This is message 1.
    //

    Request.nlmsg_seq = Sequence + 1;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));

    //
    // Test send(). This should go to the kernel by default. This is message 2.
    //

    Request.nlmsg_seq = Sequence + 2;
    LxtCheckErrno(send(Socket, &Request, sizeof(Request), 0));

    //
    // Test write(). This should go to the kernel by default. This is message 3.
    //

    Request.nlmsg_seq = Sequence + 3;
    LxtCheckErrno(write(Socket, &Request, sizeof(Request)));

    //
    // Test sendmsg() with NULL send address. This is message 4.
    // A NULL send address means the name size should be ignored, so also add
    // an invalid name size to verify this.
    //

    memset(&IoVec, 0, sizeof(IoVec));
    memset(&MessageHeader, 0, sizeof(MessageHeader));
    IoVec[0].iov_base = &Request;
    IoVec[0].iov_len = sizeof(Request);
    MessageHeader.msg_iov = IoVec;
    MessageHeader.msg_iovlen = 1;
    MessageHeader.msg_namelen = -1;
    Request.nlmsg_seq = Sequence + 4;
    LxtCheckErrno(sendmsg(Socket, &MessageHeader, 0));

    //
    // Test sendmsg() with buffer split across vectors. This is message 5.
    //

    memset(&IoVec, 0, sizeof(IoVec));
    memset(&MessageHeader, 0, sizeof(MessageHeader));
    memset(&DoubleRequest, 0, sizeof(DoubleRequest));
    IoVec[0].iov_base = &DoubleRequest.nlh1;
    IoVec[0].iov_len = __builtin_offsetof(struct nlmsghdr, nlmsg_flags);
    IoVec[1].iov_base = &DoubleRequest.nlh2.nlmsg_flags;
    IoVec[1].iov_len = sizeof(struct nlmsghdr) - __builtin_offsetof(struct nlmsghdr, nlmsg_flags);

    DoubleRequest.nlh1.nlmsg_len = NLMSG_LENGTH(0);
    DoubleRequest.nlh1.nlmsg_type = NLMSG_NOOP;
    DoubleRequest.nlh2.nlmsg_flags = NLM_F_ACK;
    DoubleRequest.nlh2.nlmsg_seq = Sequence + 5;
    MessageHeader.msg_iov = IoVec;
    MessageHeader.msg_iovlen = 2;
    LxtCheckErrno(sendmsg(Socket, &MessageHeader, 0));

    //
    // Test sendmsg() with MSG_DONTWAIT flag.
    //

    memset(&IoVec, 0, sizeof(IoVec));
    memset(&MessageHeader, 0, sizeof(MessageHeader));
    IoVec[0].iov_base = &Request;
    IoVec[0].iov_len = sizeof(Request);
    MessageHeader.msg_iov = IoVec;
    MessageHeader.msg_iovlen = 1;
    Request.nlmsg_seq = Sequence + 6;
    LxtCheckErrno(sendmsg(Socket, &MessageHeader, MSG_DONTWAIT));

    //
    // Test sendmsg() with invalid send address.
    //

    AddressLength = sizeof(SendAddress);
    memset(&SendAddress, 0, sizeof(SendAddress));
    SendAddress.nl_family = AF_NETLINK;
    SendAddress.nl_pid = 0xFFFF1234;
    MessageHeader.msg_name = &SendAddress;
    MessageHeader.msg_namelen = AddressLength;
    LxtCheckErrnoFailure(sendmsg(Socket, &MessageHeader, 0), ECONNREFUSED);

    //
    // Test sendmsg() with negative send address length.
    //

    AddressLength = sizeof(SendAddress);
    memset(&SendAddress, 0, sizeof(SendAddress));
    SendAddress.nl_family = AF_NETLINK;
    SendAddress.nl_pid = 0xFFFF1234;
    MessageHeader.msg_name = &SendAddress;
    MessageHeader.msg_namelen = -1;
    LxtCheckErrnoFailure(sendmsg(Socket, &MessageHeader, 0), EINVAL);

    //
    // Test sendmsg() with 0 nl_pid in send address.
    //

    AddressLength = sizeof(SendAddress);
    memset(&SendAddress, 0, sizeof(SendAddress));
    SendAddress.nl_family = AF_NETLINK;
    SendAddress.nl_pid = 0;
    MessageHeader.msg_name = &SendAddress;
    MessageHeader.msg_namelen = AddressLength;
    Request.nlmsg_seq = Sequence + 7;
    LxtCheckErrno(sendmsg(Socket, &MessageHeader, 0));

    //
    // Test read(). Verify the received contents. The response should be message 0.
    //

    memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    LxtCheckErrno(ReceiveResult = read(Socket, ReceiveBuffer, sizeof(ReceiveBuffer)));
    ExpectedReceiveLength = NLMSG_LENGTH(sizeof(struct nlmsgerr));
    LxtCheckEqual(ReceiveResult, ExpectedReceiveLength, "%d");
    ReceiveHeader = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckEqual(ReceiveHeader->nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_flags, 0, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_seq, Sequence, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_pid, getpid(), "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(ReceiveBuffer);
    LxtCheckEqual(Error->error, 0, "%d");

    //
    // Test recv(). Verify the received contents. The response should be message 1.
    //

    memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    ExpectedReceiveLength = NLMSG_LENGTH(sizeof(struct nlmsgerr));
    LxtCheckEqual(ReceiveResult, ExpectedReceiveLength, "%d");
    ReceiveHeader = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckEqual(ReceiveHeader->nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_flags, 0, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_seq, Sequence + 1, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_pid, getpid(), "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(ReceiveBuffer);
    LxtCheckEqual(Error->error, 0, "%d");

    //
    // Test recvfrom() with NULL receive address. Use MSG_PEEK flag, which
    // should not remove the data from the socket. Use MSG_TRUNC flag, which
    // should have no effect since the passed in buffer is larger than the
    // response. Use MSG_WAITALL flag, which has no effect on datagram sockets.
    // Verify the received contents. The response should be message 2.
    //

    memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    LxtCheckErrno(ReceiveResult = recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_PEEK | MSG_TRUNC | MSG_WAITALL, NULL, 0));

    ExpectedReceiveLength = NLMSG_LENGTH(sizeof(struct nlmsgerr));
    LxtCheckEqual(ReceiveResult, ExpectedReceiveLength, "%d");
    ReceiveHeader = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckEqual(ReceiveHeader->nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_flags, 0, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_seq, Sequence + 2, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_pid, getpid(), "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(ReceiveBuffer);
    LxtCheckEqual(Error->error, 0, "%d");

    //
    // Test recvfrom(). Use MSG_TRUNC flag. Even though the passed in
    // receive buffer is too small, the return value should be the full length
    // of the response. The response should still be message 2, since the previous
    // call was a MSK_PEEK.
    //

    memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    LxtCheckErrno(ReceiveResult = recvfrom(Socket, ReceiveBuffer, 2 * sizeof(uint32_t), MSG_TRUNC, NULL, 0));

    ExpectedReceiveLength = NLMSG_LENGTH(sizeof(struct nlmsgerr));
    LxtCheckEqual(ReceiveResult, ExpectedReceiveLength, "%d");
    ReceiveHeader = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckEqual(ReceiveHeader->nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_flags, 0, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_seq, 0, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_pid, 0, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(ReceiveBuffer);
    LxtCheckEqual(Error->error, 0, "%d");

    //
    // Test recvfrom() with 0 buffer length. This should still advance the
    // socket's internal receive buffer (making the next response message 4).
    // This response should be message 3.
    //

    memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    LxtCheckErrno(ReceiveResult = recvfrom(Socket, ReceiveBuffer, 0, 0, NULL, 0));

    ExpectedReceiveLength = 0;
    LxtCheckEqual(ReceiveResult, ExpectedReceiveLength, "%d");
    ReceiveHeader = (struct nlmsghdr*)ReceiveBuffer;

    //
    // Test recvfrom() with valid receive address. Verify the received contents.
    // The response should be message 4.
    //

    memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    memset(&ReceiveAddress, 0, sizeof(ReceiveAddress));
    AddressLength = sizeof(struct sockaddr_nl);
    LxtCheckErrno(ReceiveResult = recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0, (struct sockaddr*)&ReceiveAddress, &AddressLength));

    ExpectedReceiveLength = NLMSG_LENGTH(sizeof(struct nlmsgerr));
    LxtCheckEqual(ReceiveResult, ExpectedReceiveLength, "%d");
    LxtCheckEqual(AddressLength, sizeof(struct sockaddr_nl), "%d");
    ReceiveHeader = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckEqual(ReceiveHeader->nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_flags, 0, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_seq, Sequence + 4, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_pid, getpid(), "%d");
    LxtCheckEqual(ReceiveAddress.nl_family, AF_NETLINK, "%d");
    LxtCheckEqual(ReceiveAddress.nl_pid, 0, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(ReceiveBuffer);
    LxtCheckEqual(Error->error, 0, "%d");
    Request.nlmsg_seq = Sequence + 4;
    LxtCheckEqual(memcmp(&Error->msg, &Request, sizeof(Request)), 0, "%d");

    //
    // Test recvmsg() with valid receive address. Verify the received contents.
    //

    memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    memset(&ReceiveAddress, 0, sizeof(ReceiveAddress));
    memset(&IoVec, 0, sizeof(IoVec));
    memset(&MessageHeader, 0, sizeof(MessageHeader));
    IoVec[0].iov_base = &ReceiveBuffer;
    IoVec[0].iov_len = sizeof(ReceiveBuffer);
    MessageHeader.msg_iov = IoVec;
    MessageHeader.msg_iovlen = 1;
    MessageHeader.msg_name = &ReceiveAddress;
    MessageHeader.msg_namelen = AddressLength;

    //
    // Set some random value as the control length and check whether it gets
    // properly (re)set by the kernel.
    //

    MessageHeader.msg_controllen = 100;
    LxtCheckErrno(ReceiveResult = recvmsg(Socket, &MessageHeader, 0));
    LxtCheckEqual(MessageHeader.msg_controllen, 0, "%d");
    LxtCheckEqual(MessageHeader.msg_flags, 0, "%d");
    ExpectedReceiveLength = NLMSG_LENGTH(sizeof(struct nlmsgerr));
    LxtCheckEqual(ReceiveResult, ExpectedReceiveLength, "%d");
    LxtCheckEqual(MessageHeader.msg_namelen, AddressLength, "%d");
    ReceiveHeader = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckEqual(ReceiveHeader->nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_flags, 0, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_seq, Sequence + 5, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_pid, getpid(), "%d");
    LxtCheckEqual(ReceiveAddress.nl_family, AF_NETLINK, "%d");
    LxtCheckEqual(ReceiveAddress.nl_pid, 0, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(ReceiveBuffer);
    LxtCheckEqual(Error->error, 0, "%d");
    Request.nlmsg_seq = Sequence + 5;
    LxtCheckEqual(memcmp(&Error->msg, &Request, sizeof(Request)), 0, "%d");

    //
    // Test recvmsg() with passing in a small receive buffer.
    // The response should be truncated.
    //

    memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    memset(&IoVec, 0, sizeof(IoVec));
    memset(&MessageHeader, 0, sizeof(MessageHeader));
    IoVec[0].iov_base = &ReceiveBuffer;
    IoVec[0].iov_len = 2 * sizeof(uint32_t);
    MessageHeader.msg_iov = IoVec;
    MessageHeader.msg_iovlen = 1;
    LxtCheckErrno(ReceiveResult = recvmsg(Socket, &MessageHeader, 0));
    ExpectedReceiveLength = IoVec[0].iov_len;
    LxtCheckEqual(ReceiveResult, ExpectedReceiveLength, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_flags, 0, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_seq, 0, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_pid, 0, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(ReceiveBuffer);
    LxtCheckEqual(Error->error, 0, "%d");
    memset(&Request, 0, sizeof(Request));
    LxtCheckEqual(memcmp(&Error->msg, &Request, sizeof(Request)), 0, "%d");

    //
    // Test recvmsg() split across vectors.
    //

    memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    memset(&IoVec, 0, sizeof(IoVec));
    memset(&MessageHeader, 0, sizeof(MessageHeader));
    ReceiveHeader2 =
        (struct nlmsghdr*)&ReceiveBuffer[NLMSG_LENGTH(sizeof(struct nlmsgerr)) + __builtin_offsetof(struct nlmsghdr, nlmsg_flags)];

    IoVec[0].iov_len = __builtin_offsetof(struct nlmsghdr, nlmsg_flags);
    IoVec[0].iov_base = ReceiveHeader2;
    IoVec[1].iov_base = ReceiveBuffer + __builtin_offsetof(struct nlmsghdr, nlmsg_flags);
    IoVec[1].iov_len = NLMSG_LENGTH(sizeof(struct nlmsgerr));
    MessageHeader.msg_iov = IoVec;
    MessageHeader.msg_iovlen = 2;
    LxtCheckErrno(ReceiveResult = recvmsg(Socket, &MessageHeader, 0));
    LxtCheckEqual(MessageHeader.msg_controllen, 0, "%d");
    LxtCheckEqual(MessageHeader.msg_flags, 0, "%d");
    ExpectedReceiveLength = NLMSG_LENGTH(sizeof(struct nlmsgerr));
    LxtCheckEqual(ReceiveResult, ExpectedReceiveLength, "%d");
    ReceiveHeader = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckEqual(ReceiveHeader->nlmsg_len, 0, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_type, 0, "%d");
    ReceiveHeader->nlmsg_len = ReceiveHeader2->nlmsg_len;
    ReceiveHeader->nlmsg_type = ReceiveHeader2->nlmsg_type;
    LxtCheckEqual(ReceiveHeader->nlmsg_len, ExpectedReceiveLength, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_flags, 0, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_seq, Sequence + 7, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_pid, getpid(), "%d");
    LxtCheckEqual(ReceiveAddress.nl_family, AF_NETLINK, "%d");
    LxtCheckEqual(ReceiveAddress.nl_pid, 0, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(ReceiveBuffer);
    LxtCheckEqual(Error->error, 0, "%d");
    Request.nlmsg_len = NLMSG_LENGTH(0);
    Request.nlmsg_type = NLMSG_NOOP;
    Request.nlmsg_flags = NLM_F_ACK;
    Request.nlmsg_seq = Sequence + 7;
    LxtCheckEqual(memcmp(&Error->msg, &Request, sizeof(Request)), 0, "%d");

    //
    // Test recvfrom() when using the MSG_DONTWAIT flag. The socket has no data,
    // so it should return EAGAIN immediately instead of blocking forever.
    //

    memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    LxtCheckErrnoFailure(recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT, NULL, 0), EAGAIN);

    //
    // Test specifying that the input buffer is much larger than it actually is.
    //

    memset(&DoubleRequest, 0, sizeof(DoubleRequest));
    memset(&IoVec, 0, sizeof(IoVec));
    memset(&MessageHeader, 0, sizeof(MessageHeader));
    DoubleRequest.nlh1.nlmsg_len = 0x30000;
    DoubleRequest.nlh1.nlmsg_type = NLMSG_NOOP;
    DoubleRequest.nlh1.nlmsg_flags = NLM_F_ACK;
    DoubleRequest.nlh1.nlmsg_seq = Sequence;
    IoVec[0].iov_base = &DoubleRequest;
    IoVec[0].iov_len = 0x30000;
    MessageHeader.msg_iov = IoVec;
    MessageHeader.msg_iovlen = 1;
    Request.nlmsg_seq = Sequence + 4;
    LxtCheckErrnoFailure(sendmsg(Socket, &MessageHeader, 0), EFAULT);

    //
    // Test passing in garbage length values in the message headers.
    //

    DoubleRequest.nlh1.nlmsg_len = NLMSG_LENGTH(0) + 1;
    DoubleRequest.nlh2.nlmsg_len = 0x3000;
    IoVec[0].iov_len = sizeof(DoubleRequest.nlh1) + 1;
    LxtCheckErrno(sendmsg(Socket, &MessageHeader, 0));

    //
    // Again.
    //

    DoubleRequest.nlh1.nlmsg_len = 0x3000;
    DoubleRequest.nlh2.nlmsg_len = 0x3000;
    IoVec[0].iov_len = sizeof(DoubleRequest.nlh1) + 1;
    LxtCheckErrno(sendmsg(Socket, &MessageHeader, 0));

    //
    // Again.
    //

    DoubleRequest.nlh1.nlmsg_len = 0x3000;
    DoubleRequest.nlh2.nlmsg_len = NLMSG_LENGTH(0) + 1;
    IoVec[0].iov_len = sizeof(DoubleRequest.nlh1) + 1;
    LxtCheckErrno(sendmsg(Socket, &MessageHeader, 0));

    //
    // TODO: Test multi-message send with different requests
    //       bundled in the same send.
    //

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SocketNetlinkSendReceiveOverflow(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the send and receive family of APIs
    in the context of the receive buffer overflowing.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct sockaddr_nl BindAddress;
    struct nlmsgerr* Error;
    int ExpectedReceiveLength;
    int Index;
    struct sockaddr_nl ReceiveAddress;
    char ReceiveBuffer[500];
    struct nlmsghdr* ReceiveHeader;
    int ReceiveResult;
    struct nlmsghdr Request;
    int Result;
    struct sockaddr_nl SendAddress;
    int SendResult;
    uint32_t Sequence;
    int Socket;
    int SocketError;
    socklen_t SocketErrorSize;
    struct timeval Timeout;

    Result = LXT_RESULT_FAILURE;
    Socket = -1;
    Sequence = 0x3468;

    //
    // Create and bind socket. Create a NOOP request with the ACK flag.
    // Netlink should echo the same request back to us.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    memset(&Request, 0, sizeof(Request));
    Request.nlmsg_len = NLMSG_LENGTH(0);
    Request.nlmsg_type = NLMSG_NOOP;
    Request.nlmsg_flags = NLM_F_ACK;

    //
    // Overflow the receive buffer by sending 3000 events.
    // Each send should succeed with the return value being the full number of
    // bytes the user sent.
    //

    AddressLength = sizeof(SendAddress);
    memset(&SendAddress, 0, sizeof(SendAddress));
    SendAddress.nl_family = AF_NETLINK;
    SendAddress.nl_pid = 0;
    for (Index = 0; Index < 3000; Index++)
    {
        Request.nlmsg_seq = Sequence + Index;
        LxtCheckErrno(SendResult = sendto(Socket, &Request, sizeof(Request), 0, (struct sockaddr*)&SendAddress, AddressLength));

        LxtCheckEqual(SendResult, sizeof(Request), "%d");
    }

    //
    // The first call to recvfrom() after the overflow should return ENOBUFS.
    // Also verify that the receive and address buffers were not changed.
    //

    memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    memset(&ReceiveAddress, 0, sizeof(ReceiveAddress));
    ReceiveBuffer[1] = 0x56;
    ReceiveAddress.nl_groups = 0x3456789a;
    AddressLength = sizeof(struct sockaddr_nl);
    LxtCheckErrnoFailure(recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0, (struct sockaddr*)&ReceiveAddress, &AddressLength), ENOBUFS);

    LxtCheckEqual(ReceiveBuffer[1], 0x56, "%d");
    LxtCheckEqual(ReceiveAddress.nl_groups, 0x3456789a, "%d");

    //
    // The subsequent calls to recvfrom() pull out the responses before the
    // overflow happened. Only call recvfrom() 3 times here, so that the receive
    // buffer is not yet fully drained.
    //

    for (Index = 0; Index < 3; Index++)
    {
        memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
        memset(&ReceiveAddress, 0, sizeof(ReceiveAddress));
        LxtCheckErrno(ReceiveResult = recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0, (struct sockaddr*)&ReceiveAddress, &AddressLength));

        ExpectedReceiveLength = NLMSG_LENGTH(sizeof(struct nlmsgerr));
        LxtCheckEqual(ReceiveResult, ExpectedReceiveLength, "%d");
        LxtCheckEqual(AddressLength, sizeof(struct sockaddr_nl), "%d");
        ReceiveHeader = (struct nlmsghdr*)ReceiveBuffer;
        LxtCheckEqual(ReceiveHeader->nlmsg_type, NLMSG_ERROR, "%d");
        LxtCheckEqual(ReceiveHeader->nlmsg_flags, 0, "%d");
        LxtCheckEqual(ReceiveHeader->nlmsg_seq, Sequence + Index, "%d");
        LxtCheckEqual(ReceiveHeader->nlmsg_pid, getpid(), "%d");
        LxtCheckEqual(ReceiveAddress.nl_family, AF_NETLINK, "%d");
        LxtCheckEqual(ReceiveAddress.nl_pid, 0, "%d");
    }

    //
    // Check the socket option SO_ERROR. This should be 0 (no error),
    // since the error was cleared when the first recvfrom() returned ENOBUFS.
    //

    SocketErrorSize = sizeof(SocketError);
    SocketError = 212;
    LxtCheckErrno(getsockopt(Socket, SOL_SOCKET, SO_ERROR, &SocketError, &SocketErrorSize));

    LxtCheckEqual(SocketErrorSize, sizeof(SocketError), "%d");
    LxtCheckEqual(SocketError, 0, "%d");

    //
    // Test that the receive buffer is still considered to be "overflown"
    // until the entire buffer is drained. This means that anything sent to
    // the socket now will be ignored and no response generated, even though
    // technically there is space in the receive buffer for the response.
    // The test sends a message with a unique sequence number. Later on, the
    // entire receive buffer will be drained and all responses checked to
    // verify that this unique sequence number is not in any of the responses.
    //

    Request.nlmsg_seq = 0x98765432;
    LxtCheckErrno(SendResult = sendto(Socket, &Request, sizeof(Request), 0, (struct sockaddr*)&SendAddress, AddressLength));

    LxtCheckEqual(SendResult, sizeof(Request), "%d");

    //
    // Drain the entire response buffer. Set the recvfrom() timeout to 1 millisecond
    // so that it does not block infinitely.
    //

    Timeout.tv_sec = 0;
    Timeout.tv_usec = 1000;
    LxtCheckErrno(setsockopt(Socket, SOL_SOCKET, SO_RCVTIMEO, &Timeout, sizeof(Timeout)));

    for (;;)
    {
        memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
        memset(&ReceiveAddress, 0, sizeof(ReceiveAddress));
        ReceiveResult = recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0, (struct sockaddr*)&ReceiveAddress, &AddressLength);

        if (ReceiveResult == -1)
        {
            LxtCheckErrnoFailure(ReceiveResult, EAGAIN);
            break;
        }
        else
        {
            ExpectedReceiveLength = NLMSG_LENGTH(sizeof(struct nlmsgerr));
            LxtCheckEqual(ReceiveResult, ExpectedReceiveLength, "%d");
            LxtCheckEqual(AddressLength, sizeof(struct sockaddr_nl), "%d");
            ReceiveHeader = (struct nlmsghdr*)ReceiveBuffer;
            LxtCheckEqual(ReceiveHeader->nlmsg_type, NLMSG_ERROR, "%d");
            LxtCheckEqual(ReceiveHeader->nlmsg_flags, 0, "%d");
            LxtCheckNotEqual(ReceiveHeader->nlmsg_seq, 0, "%d");
            LxtCheckNotEqual(ReceiveHeader->nlmsg_seq, 0x98765432, "%d");
            LxtCheckEqual(ReceiveHeader->nlmsg_pid, getpid(), "%d");
            LxtCheckEqual(ReceiveAddress.nl_family, AF_NETLINK, "%d");
            LxtCheckEqual(ReceiveAddress.nl_pid, 0, "%d");
        }
    }

    //
    // Overflow the receive buffer again by sending 3000 events.
    // Each send should succeed with the return value being the full number of
    // bytes the user sent.
    //

    AddressLength = sizeof(SendAddress);
    memset(&SendAddress, 0, sizeof(SendAddress));
    SendAddress.nl_family = AF_NETLINK;
    SendAddress.nl_pid = 0;
    for (Index = 0; Index < 3000; Index++)
    {
        Request.nlmsg_seq = Sequence + Index;
        LxtCheckErrno(SendResult = sendto(Socket, &Request, sizeof(Request), 0, (struct sockaddr*)&SendAddress, AddressLength));

        LxtCheckEqual(SendResult, sizeof(Request), "%d");
    }

    //
    // Check the socket option SO_ERROR. This should be ENOBUFS,
    // since recvfrom() has not been called yet.
    //

    SocketErrorSize = sizeof(SocketError);
    SocketError = 212;
    LxtCheckErrno(getsockopt(Socket, SOL_SOCKET, SO_ERROR, &SocketError, &SocketErrorSize));

    LxtCheckEqual(SocketErrorSize, sizeof(SocketError), "%d");
    LxtCheckEqual(SocketError, ENOBUFS, "%d");

    //
    // The first recvfrom() should be successful, since the ENOBUFS error
    // was already cleared when socket option SO_ERROR was retrieved.
    //

    memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    memset(&ReceiveAddress, 0, sizeof(ReceiveAddress));
    LxtCheckErrno(ReceiveResult = recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0, (struct sockaddr*)&ReceiveAddress, &AddressLength));

    ExpectedReceiveLength = NLMSG_LENGTH(sizeof(struct nlmsgerr));
    LxtCheckEqual(ReceiveResult, ExpectedReceiveLength, "%d");
    LxtCheckEqual(AddressLength, sizeof(struct sockaddr_nl), "%d");
    ReceiveHeader = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckEqual(ReceiveHeader->nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_flags, 0, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_seq, Sequence, "%d");
    LxtCheckEqual(ReceiveHeader->nlmsg_pid, getpid(), "%d");
    LxtCheckEqual(ReceiveAddress.nl_family, AF_NETLINK, "%d");
    LxtCheckEqual(ReceiveAddress.nl_pid, 0, "%d");

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SocketNetlinkBlockedReader(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests that blocked readers do not get unblocked when the socket
    is closed. Also verify that shutdown() is invalid on NETLINK sockets.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct sockaddr_nl BindAddress;
    void* PthreadResult;
    int Result;
    int Socket;
    pthread_t Thread;
    struct timespec Timeout = {0};

    Result = LXT_RESULT_FAILURE;
    Socket = -1;

    //
    // Create and bind socket.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    //
    // Create a reader thread that will block on 'recv'.
    //

    LxtCheckResultError(pthread_create(&Thread, NULL, SocketBlockedReaderThread, &Socket));

    //
    // Wait for sometime to allow the reader thread to block on read. There
    // is no other elegant way of knowing whether the thread has blocked.
    //

    usleep(5000);

    //
    // Shutdown is NOT supported for NETLINK sockets.
    //

    LxtCheckErrnoFailure(shutdown(Socket, SHUT_RD), EOPNOTSUPP);
    LxtCheckErrnoFailure(shutdown(Socket, SHUT_WR), EOPNOTSUPP);
    LxtCheckErrnoFailure(shutdown(Socket, SHUT_RDWR), EOPNOTSUPP);

    //
    // Closing the socket does not unblock the reader. The pthread_timedjoin_np()
    // should timeout since the reader thread is still blocked.
    //

    LxtCheckErrnoZeroSuccess(close(Socket));
    usleep(5000);
    Timeout.tv_nsec = 5000;
    Result = pthread_timedjoin_np(Thread, &PthreadResult, &Timeout);
    if (Result != ETIMEDOUT)
    {
        LxtLogError(
            "Expecting pthread_tryjoin_np to return ETIMEDOUT(%d), "
            "but it returned with result: %d",
            ETIMEDOUT,
            Result);

        goto ErrorExit;
    }

    //
    // No other choice but to kill the reader thread.
    //

    LxtCheckErrnoZeroSuccess(pthread_cancel(Thread));
    LxtCheckErrnoZeroSuccess(pthread_join(Thread, &PthreadResult));
    LxtCheckEqual(PthreadResult, PTHREAD_CANCELED, "%p");

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SocketNetlinkEpoll(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests that the epoll state is correct.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct sockaddr_nl BindAddress;
    int EdRead;
    int EdWrite;
    struct epoll_event EpollControlEvent;
    struct epoll_event EpollWaitEvent[2];
    int Index;
    char ReceiveBuffer[500];
    int ReceiveResult;
    struct nlmsghdr Request;
    int Result;
    int SendResult;
    int Socket;

    Result = LXT_RESULT_FAILURE;
    Socket = -1;

    //
    // Create socket.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK, 0));

    //
    // Create epoll containers for read and write and
    // add the socket descriptor to them.
    //

    LxtCheckErrno(EdRead = epoll_create(1));
    EpollControlEvent.events = EPOLLIN;
    EpollControlEvent.data.fd = Socket;
    Result = epoll_ctl(EdRead, EPOLL_CTL_ADD, Socket, &EpollControlEvent);
    LxtCheckErrnoZeroSuccess(Result);

    LxtCheckErrno(EdWrite = epoll_create(1));
    EpollControlEvent.events = EPOLLOUT;
    EpollControlEvent.data.fd = Socket;
    Result = epoll_ctl(EdWrite, EPOLL_CTL_ADD, Socket, &EpollControlEvent);
    LxtCheckErrnoZeroSuccess(Result);

    //
    // Wait for data to be available with a timeout. This should timeout since
    // there is no data. Verify that write is available.
    //

    Result = epoll_wait(EdRead, EpollWaitEvent, 2, 50);
    LxtCheckEqual(Result, 0, "%d");
    Result = epoll_wait(EdWrite, EpollWaitEvent, 2, 50);
    LxtCheckEqual(Result, 1, "%d");

    //
    // Bind the socket.
    //

    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    //
    // Wait for data to be available with a timeout. This should timeout since
    // there is no data. Verify that write is available.
    //

    Result = epoll_wait(EdRead, EpollWaitEvent, 2, 50);
    LxtCheckEqual(Result, 0, "%d");
    Result = epoll_wait(EdWrite, EpollWaitEvent, 2, 50);
    LxtCheckEqual(Result, 1, "%d");

    //
    // Send 3000 messages. The receive buffer will overflow, but write is still
    // available. Read is available now.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlmsg_len = NLMSG_LENGTH(0);
    Request.nlmsg_type = NLMSG_NOOP;
    Request.nlmsg_flags = NLM_F_ACK;
    for (Index = 0; Index < 3000; Index++)
    {
        LxtCheckErrno(SendResult = sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));

        LxtCheckEqual(SendResult, sizeof(Request), "%d");
    }

    Result = epoll_wait(EdRead, EpollWaitEvent, 2, 50);
    LxtCheckEqual(Result, 1, "%d");
    Result = epoll_wait(EdWrite, EpollWaitEvent, 2, 50);
    LxtCheckEqual(Result, 1, "%d");

    //
    // Drain all the events. Write is still available, but read is not.
    //

    LxtCheckErrnoFailure(recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0, NULL, 0), ENOBUFS);

    for (;;)
    {
        memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
        ReceiveResult = recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0, NULL, 0);

        if (ReceiveResult == -1)
        {
            LxtCheckErrnoFailure(ReceiveResult, EAGAIN);
            break;
        }
    }

    Result = epoll_wait(EdRead, EpollWaitEvent, 2, 50);
    LxtCheckEqual(Result, 0, "%d");
    Result = epoll_wait(EdWrite, EpollWaitEvent, 2, 50);
    LxtCheckEqual(Result, 1, "%d");

    //
    // Close the socket. Both read and write are not available.
    //

    LxtCheckErrnoZeroSuccess(close(Socket));
    Result = epoll_wait(EdRead, EpollWaitEvent, 2, 50);
    LxtCheckEqual(Result, 0, "%d");
    Result = epoll_wait(EdWrite, EpollWaitEvent, 2, 50);
    LxtCheckEqual(Result, 0, "%d");

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    if (EdRead > 0)
    {
        close(EdRead);
    }

    if (EdWrite > 0)
    {
        close(EdWrite);
    }

    return Result;
}

int SocketNetlinkRecvmmsg(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the recvmmsg() syscall.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct sockaddr_nl BindAddress;
    NetlinkRecvmmsgBlockedReaderParams BlockedReaderParams;
    struct nlmsgerr* Error;
    struct nlmsghdr* Header;
    int Index;
    socklen_t OptionLength;
    void* PthreadResult;
    char ReceiveBuffers[10][1000];
    struct iovec ReceiveIovecs[10];
    struct mmsghdr ReceiveMessages[10];
    int ReceiveResult;
    int Result;
    int SendResult;
    int Socket;
    pthread_t Thread;
    struct timespec Timeout;
    struct timespec TimeoutPthread;

    struct
    {
        struct nlmsghdr nlh;
        struct ifinfomsg ifm;
        struct rtattr ext_req __attribute__((aligned(NLMSG_ALIGNTO)));
        __u32 ext_filter_mask;
    } Request;

    //
    // Create and bind socket. Create a RTM_GETLINK request. Note that the
    // request is not sent yet at this point.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_DUMP;
    Request.nlh.nlmsg_len = sizeof(Request);
    Request.nlh.nlmsg_type = RTM_GETLINK;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.ifm.ifi_family = AF_NETLINK;
    Request.ext_req.rta_type = IFLA_EXT_MASK;
    Request.ext_req.rta_len = RTA_LENGTH(sizeof(__u32));
    Request.ext_filter_mask = RTEXT_FILTER_VF;

    //
    // The timeout value passed to recvmmsg() is ignored - there is a bug in the
    // Linux kernel where the timeout does not work at all. In lxcore we ignore
    // this parameter.
    //

    Timeout.tv_sec = 800;
    Timeout.tv_nsec = 0;

    //
    // Set up the receive buffers.
    //

    memset(ReceiveMessages, 0, sizeof(ReceiveMessages));
    for (Index = 0; Index < 10; Index++)
    {
        ReceiveIovecs[Index].iov_base = ReceiveBuffers[Index];
        ReceiveIovecs[Index].iov_len = 1000;
        ReceiveMessages[Index].msg_hdr.msg_iov = &ReceiveIovecs[Index];
        ReceiveMessages[Index].msg_hdr.msg_iovlen = 1;
    }

    //
    // There is nothing to receive at this point. Test that MSG_DONTWAIT trumps
    // MSG_WAITFORONE and MSG_WAITALL, therefore EAGAIN is returned due to not waiting.
    //

    LxtCheckErrnoFailure(Result = recvmmsg(Socket, ReceiveMessages, 1000, MSG_DONTWAIT | MSG_WAITFORONE | MSG_WAITALL, &Timeout), EAGAIN);

    LxtCheckErrnoFailure(Result = recvmmsg(Socket, ReceiveMessages, 1000, MSG_DONTWAIT | MSG_WAITFORONE, &Timeout), EAGAIN);

    LxtCheckErrnoFailure(Result = recvmmsg(Socket, ReceiveMessages, 1000, MSG_DONTWAIT | MSG_WAITALL, &Timeout), EAGAIN);

    LxtCheckErrnoFailure(Result = recvmmsg(Socket, ReceiveMessages, 1000, MSG_DONTWAIT, &Timeout), EAGAIN);

    //
    // Set a SO_RCVTIMEO timeout value of 8 milliseconds for the receive. This timeout
    // applies to each individual recvmsg() call and actually works. Both calls to
    // recvmmsg below should time out from the SO_RCVTIMEO.
    //

    LxtCheckResult(SocketNetlinkSetAndVerifySocketOptionTimeout(Socket, SO_RCVTIMEO, 8000));

    LxtCheckErrnoFailure(Result = recvmmsg(Socket, ReceiveMessages, 1000, MSG_WAITFORONE, &Timeout), EAGAIN);

    LxtCheckErrnoFailure(Result = recvmmsg(Socket, ReceiveMessages, 1000, 0, &Timeout), EAGAIN);

    //
    // Restore the SO_RCVTIMEO timeout to 0 (never times out).
    //

    LxtCheckResult(SocketNetlinkSetAndVerifySocketOptionTimeout(Socket, SO_RCVTIMEO, 0));

    //
    // Test blocking behavior when zero flags (option 0) and when MSG_WAITFORONE
    // (option 1) is passed to recvmmsg. Create a new thread that blocks forever,
    // then cancel the thread after waiting for some time.
    //

    for (Index = 0; Index < 2; Index++)
    {

        //
        // Create a reader thread that will block on 'recvmmsg'.
        //

        BlockedReaderParams.Socket = Socket;
        BlockedReaderParams.Option = Index;
        LxtCheckResultError(pthread_create(&Thread, NULL, SocketNetlinkRecvmmsgBlockedReaderThread, &BlockedReaderParams));

        //
        // Wait for sometime to allow the reader thread to block on read. There
        // is no other elegant way of knowing whether the thread has blocked.
        //

        usleep(5000);

        //
        // The pthread_timedjoin_np() should timeout since the reader thread is
        // still blocked.
        //

        TimeoutPthread.tv_sec = 0;
        TimeoutPthread.tv_nsec = 5000;
        Result = pthread_timedjoin_np(Thread, &PthreadResult, &TimeoutPthread);
        if (Result != ETIMEDOUT)
        {
            LxtLogError(
                "Expecting pthread_tryjoin_np to return ETIMEDOUT(%d), "
                "but it returned with result: %d for index: %d",
                ETIMEDOUT,
                Result,
                Index);

            goto ErrorExit;
        }

        //
        // No other choice but to kill the reader thread.
        //

        LxtCheckErrnoZeroSuccess(pthread_cancel(Thread));
        LxtCheckErrnoZeroSuccess(pthread_join(Thread, &PthreadResult));
        LxtCheckEqual(PthreadResult, PTHREAD_CANCELED, "%p");
    }

    //
    // Now generate a lot of Netlink responses waiting to be read.
    //

    for (Index = 0; Index < 10; Index++)
    {
        LxtCheckErrno(SendResult = send(Socket, &Request, sizeof(Request), 0));
        LxtCheckEqual(SendResult, sizeof(Request), "%d");
    }

    //
    // Test various combinations of input parameters and verify the output.
    //

    LxtCheckErrno(Result = recvmmsg(Socket, ReceiveMessages, 3, MSG_DONTWAIT | MSG_WAITFORONE | MSG_WAITALL | MSG_PEEK | MSG_TRUNC, &Timeout));

    LxtCheckEqual(Result, 3, "%d");
    LxtCheckNotEqual(ReceiveMessages[0].msg_len, 0, "%d");
    LxtCheckNotEqual(ReceiveMessages[1].msg_len, 0, "%d");
    LxtCheckNotEqual(ReceiveMessages[2].msg_len, 0, "%d");
    LxtCheckEqual(ReceiveMessages[3].msg_len, 0, "%d");
    LxtCheckErrno(Result = recvmmsg(Socket, ReceiveMessages, 2, MSG_DONTWAIT, &Timeout));
    LxtCheckEqual(Result, 2, "%d");
    LxtCheckNotEqual(ReceiveMessages[0].msg_len, 0, "%d");
    LxtCheckNotEqual(ReceiveMessages[1].msg_len, 0, "%d");
    LxtCheckNotEqual(ReceiveMessages[2].msg_len, 0, "%d");
    LxtCheckEqual(ReceiveMessages[3].msg_len, 0, "%d");
    ReceiveMessages[0].msg_len = 0;
    ReceiveMessages[1].msg_len = 0;
    ReceiveMessages[2].msg_len = 0;
    LxtCheckErrno(Result = recvmmsg(Socket, ReceiveMessages, 1, 0, &Timeout));
    LxtCheckEqual(Result, 1, "%d");
    LxtCheckNotEqual(ReceiveMessages[0].msg_len, 0, "%d");
    LxtCheckEqual(ReceiveMessages[1].msg_len, 0, "%d");
    LxtCheckEqual(ReceiveMessages[2].msg_len, 0, "%d");
    LxtCheckEqual(ReceiveMessages[3].msg_len, 0, "%d");
    LxtCheckErrno(Result = recvmmsg(Socket, ReceiveMessages, 1, MSG_WAITFORONE, &Timeout));
    LxtCheckEqual(Result, 1, "%d");
    LxtCheckNotEqual(ReceiveMessages[0].msg_len, 0, "%d");
    LxtCheckEqual(ReceiveMessages[1].msg_len, 0, "%d");
    LxtCheckEqual(ReceiveMessages[2].msg_len, 0, "%d");
    LxtCheckEqual(ReceiveMessages[3].msg_len, 0, "%d");
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

void* SocketNetlinkRecvmmsgBlockedReaderThread(void* Arg)

/*++

Routine Description:

    This routine will call recvmmsg on the given socket fd and block.

Arguments:

    Arg - Supplies the socket fd to read and the options.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    NetlinkRecvmmsgBlockedReaderParams* BlockedReaderParams;
    int Flags;
    int Index;
    int MessagesRead;
    char ReceiveBuffers[20][1000];
    struct iovec ReceiveIovecs[20];
    struct mmsghdr ReceiveMessages[20];
    int Result = LXT_RESULT_FAILURE;
    int Socket;
    struct timespec Timeout;

    //
    // The timeout value passed to recvmmsg() is ignored. Set up the receive
    // buffers.
    //

    BlockedReaderParams = (NetlinkRecvmmsgBlockedReaderParams*)Arg;
    Socket = BlockedReaderParams->Socket;
    Timeout.tv_sec = 800;
    Timeout.tv_nsec = 0;
    memset(ReceiveMessages, 0, sizeof(ReceiveMessages));
    for (Index = 0; Index < 20; Index++)
    {
        ReceiveIovecs[Index].iov_base = ReceiveBuffers[Index];
        ReceiveIovecs[Index].iov_len = 1000;
        ReceiveMessages[Index].msg_hdr.msg_iov = &ReceiveIovecs[Index];
        ReceiveMessages[Index].msg_hdr.msg_iovlen = 1;
    }

    //
    // The flags passed to recvmmsg() depend on the option value passed into
    // this function.
    //

    switch (BlockedReaderParams->Option)
    {
    case 0:
        Flags = 0;
        break;

    case 1:
        Flags = MSG_WAITFORONE;
        break;

    default:
        LxtLogError("Incorrect option: %d", BlockedReaderParams->Option);
        goto ErrorExit;
    }

    LxtCheckErrno(MessagesRead = recvmmsg(Socket, ReceiveMessages, 20, Flags, NULL));
    if (MessagesRead != 0)
    {
        LxtLogError(
            "recvmmsg should return 0 messages read, "
            "but it returned %d messages for flags %x",
            MessagesRead,
            Flags);

        goto ErrorExit;
    }

    LxtLogInfo("recvmmsg unblocked, flags %x", Flags);
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    pthread_exit((void*)(ssize_t)Result);
}

int SocketNetlinkSendBadMessage(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends bad Netlink messages, to test that the system does not crash.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct sockaddr_nl BindAddress;
    char* Buffer;
    struct nlmsghdr* Header;
    char ReceiveBuffer;
    int Result;
    int Socket;
    int TypeIterator;

    Buffer = malloc(PAGE_SIZE * 2);
    LxtCheckTrue(Buffer != NULL);
    Header = (struct nlmsghdr*)(((uint64_t)(Buffer + PAGE_SIZE) & ~(PAGE_SIZE - 1)) - sizeof(struct nlmsghdr));
    LxtLogInfo("Malloc end of page: %p", Header);
    for (TypeIterator = 0; TypeIterator < sizeof(MessageTypes) / sizeof(int); TypeIterator++)
    {

        LxtLogInfo("Checking message type: %d", MessageTypes[TypeIterator]);
        LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
        memset(&BindAddress, 0, sizeof(BindAddress));
        BindAddress.nl_family = AF_NETLINK;
        AddressLength = sizeof(BindAddress);
        LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

        Header->nlmsg_flags = NLM_F_REQUEST;
        Header->nlmsg_len = sizeof(struct nlmsghdr);
        Header->nlmsg_type = MessageTypes[TypeIterator];
        Header->nlmsg_seq = 0x4567;
        sendto(Socket, Header, sizeof(struct nlmsghdr), 0, NULL, 0);
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    if (Buffer != NULL)
    {
        free(Buffer);
    }

    return Result;
}

void SocketNetlinkRouteDumpAttributeData(char* Buffer, int BufferSize, struct rtattr* Attribute)

/*++

Routine Description:

    This routine dumps the data in the NETLINK_ROUTE protocol's RT attribute.

Arguments:

    Buffer - Supplies a pointer to the buffer to write the dumped data to.

    Attribute - Supplies a pointer to the RT attribute to dump.

Return Value:

    None.

--*/

{

    unsigned char* AttributeCurrent;
    char* BufferCurrent;
    int Index;

    AttributeCurrent = (unsigned char*)RTA_DATA(Attribute);
    BufferCurrent = Buffer;
    memset(Buffer, 0, BufferSize);

    //
    // Each char takes 3 bytes in the dump buffer.
    //

    for (Index = 0; Index < min((BufferSize - 1) / 3, RTA_PAYLOAD(Attribute)); Index++)
    {
        sprintf(BufferCurrent, "%02x ", *AttributeCurrent);
        AttributeCurrent += 1;
        BufferCurrent += 3;
    }

    return;
}

int SocketNetlinkRouteGetAddr(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the NETLINK_ROUTE protocol's RTM_GETADDR message.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct rtattr* Attribute;
    char AttributeDump[ATTRIBUTE_DUMP_BUFFER_SIZE];
    int AttributeSeenAddress;
    int AttributeSeenCacheInfo;
    struct sockaddr_nl BindAddress;
    struct nlmsgerr* Error;
    int FoundDone;
    struct nlmsghdr* Header;
    struct ifaddrmsg* IfAddrMsg;
    char ReceiveBuffer[5000];
    int ReceiveResult;
    int RemainingLength;
    int Result;
    int Socket;

    struct
    {
        struct nlmsghdr nlh;
        struct ifinfomsg ifm;
        struct rtattr ext_req __attribute__((aligned(NLMSG_ALIGNTO)));
        __u32 ext_filter_mask;
    } Request;

    AttributeSeenAddress = 0;
    AttributeSeenCacheInfo = 0;

    //
    // Create and bind socket. Create a RTM_GETADDR request.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = sizeof(Request);
    Request.nlh.nlmsg_type = RTM_GETADDR;
    Request.nlh.nlmsg_seq = 0x4563;
    Request.ifm.ifi_family = AF_NETLINK;
    Request.ext_req.rta_type = IFLA_EXT_MASK;
    Request.ext_req.rta_len = RTA_LENGTH(sizeof(__u32));
    Request.ext_filter_mask = RTEXT_FILTER_VF;

    //
    // Test flags. Only passing the NLM_F_REQUEST flag returns an Error.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));

    Header = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckTrue(NLMSG_OK(Header, ReceiveResult));
    LxtCheckEqual(Header->nlmsg_type, NLMSG_ERROR, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(Header);
    LxtCheckEqual(Error->error, -EOPNOTSUPP, "%d");
    LxtCheckEqual(memcmp(&Error->msg, &Request, sizeof(Request)), 0, "%d");

    //
    // Test flags. Only passing the NLM_F_REQUEST flag returns an Error.
    // Verify that adding a NLM_F_ACK flag still returns an Error.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));

    Header = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckTrue(NLMSG_OK(Header, ReceiveResult));
    LxtCheckEqual(Header->nlmsg_type, NLMSG_ERROR, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(Header);
    LxtCheckEqual(Error->error, -EOPNOTSUPP, "%d");
    LxtCheckEqual(memcmp(&Error->msg, &Request, sizeof(Request)), 0, "%d");

    //
    // Test flags. Only passing the NLM_F_ROOT and/or NLM_F_MATCH flag(s)
    // result in no response.
    //

    Request.nlh.nlmsg_flags = NLM_F_ROOT;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = NLM_F_MATCH;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = NLM_F_ROOT | NLM_F_MATCH;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Test flags. Passing 0 flags or invalid flags result in no response.
    //

    Request.nlh.nlmsg_flags = 0;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = 0x40;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Test flags. NLM_F_REQUEST and at least one of NLM_F_ROOT/NLM_F_MATCH
    // results in the correct response.
    // For the response, verify the presence of several attributes.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    FoundDone = 0;
    for (;;)
    {
        LxtCheckErrno(ReceiveResult = recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0, NULL, 0));

        Header = (struct nlmsghdr*)ReceiveBuffer;
        while (NLMSG_OK(Header, ReceiveResult))
        {
            if (Header->nlmsg_type == NLMSG_DONE)
            {
                FoundDone = 1;
                break;
            }

            IfAddrMsg = (struct ifaddrmsg*)NLMSG_DATA(Header);
            LxtLogInfo(
                "ifaddrmsg: ifa_family %d ifa_prefixlen %d "
                "ifa_flags %d ifa_scope %d ifa_index %d",
                IfAddrMsg->ifa_family,
                IfAddrMsg->ifa_prefixlen,
                IfAddrMsg->ifa_flags,
                IfAddrMsg->ifa_scope,
                IfAddrMsg->ifa_index);

            Attribute = (struct rtattr*)((char*)IfAddrMsg + sizeof(struct ifaddrmsg));

            RemainingLength = Header->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifaddrmsg));

            while (RTA_OK(Attribute, RemainingLength))
            {
                SocketNetlinkRouteDumpAttributeData(AttributeDump, ATTRIBUTE_DUMP_BUFFER_SIZE, Attribute);

                LxtLogInfo("RTATTR type: %2d len: %3d data: %s", Attribute->rta_type, Attribute->rta_len, AttributeDump);

                if (Attribute->rta_type == IFA_ADDRESS)
                {
                    AttributeSeenAddress += 1;
                }
                else if (Attribute->rta_type == IFA_CACHEINFO)
                {
                    LxtCheckEqual(Attribute->rta_len, RTA_LENGTH(sizeof(struct ifa_cacheinfo)), "%d");

                    AttributeSeenCacheInfo += 1;
                }

                Attribute = RTA_NEXT(Attribute, RemainingLength);
            }

            Header = NLMSG_NEXT(Header, ReceiveResult);
        }

        if (FoundDone == 1)
        {
            break;
        }
    }

    LxtCheckTrue(AttributeSeenAddress > 0);
    LxtCheckTrue(AttributeSeenCacheInfo > 0);

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SocketNetlinkRouteGetLink(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the NETLINK_ROUTE protocol's RTM_GETLINK message.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct sockaddr_nl BindAddress;
    pid_t ChildPid;
    int ChildSocket;
    struct nlmsgerr* Error;
    struct nlmsghdr* Header;
    int Index;
    char InterfaceName[32];
    int LoopbackIndex;
    char ReceiveBuffer[5000];
    int ReceiveResult;
    int Result;
    int SendResult;
    int Socket;

    struct
    {
        struct nlmsghdr nlh;
        struct ifinfomsg ifm;
        char Attributes[200];
    } Request;

    //
    // Create and bind socket. Create a RTM_GETLINK request.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = sizeof(Request);
    Request.nlh.nlmsg_type = RTM_GETLINK;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.ifm.ifi_family = AF_NETLINK;

    //
    // Test flags. Only passing the NLM_F_REQUEST flag with no network interface
    // specified returns an Error.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));

    Header = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckTrue(NLMSG_OK(Header, ReceiveResult));
    LxtCheckEqual(Header->nlmsg_type, NLMSG_ERROR, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(Header);
    LxtCheckEqual(Error->error, -EINVAL, "%d");
    LxtCheckEqual(memcmp(&Error->msg, &Request, sizeof(Request)), 0, "%d");

    //
    // Test flags. Only passing the NLM_F_REQUEST flag with no network interface
    // returns an Error. Verify that adding a NLM_F_ACK flag still returns an Error.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));

    Header = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckTrue(NLMSG_OK(Header, ReceiveResult));
    LxtCheckEqual(Header->nlmsg_type, NLMSG_ERROR, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(Header);
    LxtCheckEqual(Error->error, -EINVAL, "%d");
    LxtCheckEqual(memcmp(&Error->msg, &Request, sizeof(Request)), 0, "%d");

    //
    // Test flags. Only passing the NLM_F_ROOT and/or NLM_F_MATCH flag(s)
    // result in no response.
    //

    Request.nlh.nlmsg_flags = NLM_F_ROOT;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = NLM_F_MATCH;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = NLM_F_ROOT | NLM_F_MATCH;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Test flags. Passing 0 flags or invalid flags result in no response.
    //

    Request.nlh.nlmsg_flags = 0;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = 0x40;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Test filter mode.
    // When passing only the NLM_F_REQUEST flag, "filter" mode is used.
    // In this mode, either ifinfomsg.ifi_index must be valid, or IFLA_IFNAME
    // must be present to filter the response to one network interface.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST;

    //
    // Passing a zero ifi_index returns an Error.
    //

    Request.ifm.ifi_index = 0;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));

    Header = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckTrue(NLMSG_OK(Header, ReceiveResult));
    LxtCheckEqual(Header->nlmsg_type, NLMSG_ERROR, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(Header);
    LxtCheckEqual(Error->error, -EINVAL, "%d");

    //
    // Passing a negative ifi_index returns an Error.
    //

    Request.ifm.ifi_index = -1;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));

    Header = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckTrue(NLMSG_OK(Header, ReceiveResult));
    LxtCheckEqual(Header->nlmsg_type, NLMSG_ERROR, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(Header);
    LxtCheckEqual(Error->error, -EINVAL, "%d");

    //
    // Passing a bad ifi_index and the correct interface name returns an Error.
    //

    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    Request.ifm.ifi_index = 100;
    strcpy(InterfaceName, "lo");
    LxtCheckErrnoZeroSuccess(
        SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), IFLA_IFNAME, InterfaceName, strlen(InterfaceName) + 1));

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));

    Header = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckTrue(NLMSG_OK(Header, ReceiveResult));
    LxtCheckEqual(Header->nlmsg_type, NLMSG_ERROR, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(Header);
    LxtCheckEqual(Error->error, -ENODEV, "%d");

    //
    // Passing a negative ifi_index and the correct interface name results in the
    // correct response.
    //

    Request.ifm.ifi_index = -1;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckResult(SocketNetlinkRouteGetLinkCheckResponse(Socket, TRUE));

    //
    // Passing a zero ifi_index and the correct interface name results in the
    // correct response.
    //

    Request.ifm.ifi_index = 0;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckResult(SocketNetlinkRouteGetLinkCheckResponse(Socket, TRUE));

    //
    // Passing a bad ifi_index and a bad interface name returns an Error.
    //

    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    Request.ifm.ifi_index = 100;
    strcpy(InterfaceName, "loooo");
    LxtCheckErrnoZeroSuccess(
        SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), IFLA_IFNAME, InterfaceName, strlen(InterfaceName) + 1));

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));

    Header = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckTrue(NLMSG_OK(Header, ReceiveResult));
    LxtCheckEqual(Header->nlmsg_type, NLMSG_ERROR, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(Header);
    LxtCheckEqual(Error->error, -ENODEV, "%d");

    //
    // Passing a zero ifi_index and a bad interface name returns an Error.
    //

    Request.ifm.ifi_index = 0;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));

    Header = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckTrue(NLMSG_OK(Header, ReceiveResult));
    LxtCheckEqual(Header->nlmsg_type, NLMSG_ERROR, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(Header);
    LxtCheckEqual(Error->error, -ENODEV, "%d");

    //
    // Passing a negative ifi_index and a bad interface name returns an Error.
    //

    Request.ifm.ifi_index = -1;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));

    Header = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckTrue(NLMSG_OK(Header, ReceiveResult));
    LxtCheckEqual(Header->nlmsg_type, NLMSG_ERROR, "%d");
    Error = (struct nlmsgerr*)NLMSG_DATA(Header);
    LxtCheckEqual(Error->error, -ENODEV, "%d");

    //
    // Get the interface index of the loopback adapter.
    //

    LxtCheckErrnoZeroSuccess(SocketNetlinkGetLoopbackIndex(&LoopbackIndex));
    LxtCheckTrue(LoopbackIndex > 0);

    //
    // Passing a correct ifi_index and a bad interface name results in the
    // correct response.
    //

    Request.ifm.ifi_index = LoopbackIndex;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckResult(SocketNetlinkRouteGetLinkCheckResponse(Socket, TRUE));

    //
    // Passing a correct ifi_index and no interface name results in the
    // correct response.
    //

    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckResult(SocketNetlinkRouteGetLinkCheckResponse(Socket, TRUE));

    //
    // Test dump mode.
    // NLM_F_REQUEST and at least one of NLM_F_ROOT/NLM_F_MATCH
    // results in the correct response.
    // For the response, verify that at least one interface is present (loopback),
    // and also verify the presence of several attributes.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckResult(SocketNetlinkRouteGetLinkCheckResponse(Socket, FALSE));

    //
    // Create a child process and switch it to a new network namespace.
    // RTM_GETLINK should only return one network interface - loopback.
    //

    LxtCheckErrno(ChildPid = fork());
    if (ChildPid == 0)
    {
        LxtCheckErrnoZeroSuccess(unshare(CLONE_NEWNET));

        //
        // N.B. The sleep is because it can take some time for the lxcore cache to get
        //      the new network interface notification.
        //

        usleep(1000 * 100);
        LxtLogInfo("Now testing child socket");
        LxtCheckErrno(ChildSocket = socket(AF_NETLINK, SOCK_RAW, 0));
        LxtCheckErrno(sendto(ChildSocket, &Request, sizeof(Request), 0, NULL, 0));
        LxtCheckResult(SocketNetlinkRouteGetLinkCheckResponse(ChildSocket, TRUE));
        LxtClose(ChildSocket);
        exit(0);
    }

    LxtCheckResult(LxtWaitPidPoll(ChildPid, 0));

    //
    // Check that sending is still successful even if the
    // receive buffer has overflown.
    //

    for (Index = 0; Index < 1000; Index++)
    {
        LxtCheckErrno(SendResult = send(Socket, &Request, sizeof(Request), 0));
        LxtCheckEqual(SendResult, sizeof(Request), "%d");
    }

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SocketNetlinkRouteGetLinkCheckResponse(int Socket, int OnlyOneInterface)

/*++

Routine Description:

    This routine checks the response of the NETLINK_ROUTE protocol's
    RTM_GETLINK message.

Arguments:

    Socket - Supplies the socket to read the response from.

    OnlyOneInterface - Supplies a boolean indicating whether there should only
        be one network interface in the response.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct rtattr* Attribute;
    char AttributeDump[ATTRIBUTE_DUMP_BUFFER_SIZE];
    int AttributeSeenAddress;
    int AttributeSeenMtu;
    int AttributeSeenName;
    int FoundDone;
    struct nlmsghdr* Header;
    struct ifinfomsg* IfInfoMsg;
    int InterfaceCount;
    char ReceiveBuffer[5000];
    int ReceiveResult;
    int RemainingLength;
    int Result;
    int SeenLoopback;

    AttributeSeenAddress = 0;
    AttributeSeenMtu = 0;
    AttributeSeenName = 0;
    SeenLoopback = 0;
    FoundDone = 0;
    InterfaceCount = 0;
    for (;;)
    {
        LxtCheckErrno(ReceiveResult = recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0, NULL, 0));

        Header = (struct nlmsghdr*)ReceiveBuffer;
        while (NLMSG_OK(Header, ReceiveResult))
        {
            if (Header->nlmsg_type == NLMSG_DONE)
            {
                FoundDone = 1;
                break;
            }

            IfInfoMsg = (struct ifinfomsg*)NLMSG_DATA(Header);
            LxtLogInfo(
                "ifinfomsg: ifi_family %d ifi_type %d "
                "ifi_index %d ifi_flags %d ifi_change %d",
                IfInfoMsg->ifi_family,
                IfInfoMsg->ifi_type,
                IfInfoMsg->ifi_index,
                IfInfoMsg->ifi_flags,
                IfInfoMsg->ifi_change);

            InterfaceCount += 1;
            if ((IfInfoMsg->ifi_flags & IFF_LOOPBACK) != 0)
            {
                SeenLoopback += 1;
            }

            Attribute = (struct rtattr*)((char*)IfInfoMsg + sizeof(struct ifinfomsg));

            RemainingLength = Header->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifinfomsg));

            while (RTA_OK(Attribute, RemainingLength))
            {
                SocketNetlinkRouteDumpAttributeData(AttributeDump, ATTRIBUTE_DUMP_BUFFER_SIZE, Attribute);

                LxtLogInfo("RTATTR type: %2d len: %3d data: %s", Attribute->rta_type, Attribute->rta_len, AttributeDump);

                if (Attribute->rta_type == IFLA_ADDRESS)
                {
                    AttributeSeenAddress += 1;
                }
                else if (Attribute->rta_type == IFLA_MTU)
                {
                    AttributeSeenMtu += 1;
                }
                else if (Attribute->rta_type == IFLA_IFNAME)
                {
                    AttributeSeenName += 1;
                }

                Attribute = RTA_NEXT(Attribute, RemainingLength);
            }

            if ((Header->nlmsg_flags & NLM_F_MULTI) == 0)
            {
                goto LoopDone;
            }

            Header = NLMSG_NEXT(Header, ReceiveResult);
        }

        if (FoundDone == 1)
        {
            break;
        }
    }

LoopDone:
    LxtCheckTrue(AttributeSeenAddress > 0);
    LxtCheckTrue(AttributeSeenMtu > 0);
    LxtCheckTrue(AttributeSeenName > 0);
    LxtCheckEqual(SeenLoopback, 1, "%d");
    LxtLogInfo("Found %d interfaces total", InterfaceCount);
    if (OnlyOneInterface != FALSE)
    {
        LxtCheckEqual(InterfaceCount, 1, "%d");
    }

    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int SocketNetlinkRouteGetRouteBestRoute(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the NETLINK_ROUTE protocol's RTM_GETROUTE message's
    get best route request.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct sockaddr_nl BindAddress;
    struct in_addr DestinationIpv4;
    struct in6_addr DestinationIpv6;
    struct in_addr GatewayIpv4;
    struct in6_addr GatewayIpv6;
    struct nlmsgerr* Error;
    struct nlmsghdr* Header;
    int Index;
    bool IsIpV6Configured;
    int LoopbackIndex;
    char ReceiveBuffer[5000];
    int ReceiveResult;
    int Result;
    int SendResult;
    int Socket;

    struct
    {
        struct nlmsghdr nlh;
        struct rtmsg msg;
        char attr[200];
    } Request;

    //
    // Get the interface index of the loopback adapter.
    //

    LxtCheckErrnoZeroSuccess(SocketNetlinkGetLoopbackIndex(&LoopbackIndex));
    LxtCheckTrue(LoopbackIndex > 0);

    //
    // Create and bind socket. Create a RTM_GETROUTE request.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_GETROUTE;
    Request.nlh.nlmsg_seq = 0x4567;

    //
    // NLM_F_REQUEST with no NLM_F_DUMP means "get best route" request.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST;

    //
    // Test specifying an invalid address family.
    //

    Request.msg.rtm_family = AF_UNSPEC;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EOPNOTSUPP));

    //
    // Test specifying AF_INET6 while really giving an IPv4 destination address.
    //

    Request.msg.rtm_family = AF_INET6;
    inet_aton("1.1.1.1", &DestinationIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, NULL, -1);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EINVAL));

    IsIpV6Configured = SocketNetlinkIsIpv6Configured();

    //
    // Test not specifying the destination (both Ipv4 and Ipv6).
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_GETROUTE;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST;
    Request.msg.rtm_family = AF_INET;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckResult(SocketNetlinkRouteGetRouteBestRouteCheckResponse(Socket, FALSE));
    Request.msg.rtm_family = AF_INET6;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    if (IsIpV6Configured)
    {
        LxtCheckResult(SocketNetlinkRouteGetRouteBestRouteCheckResponse(Socket, FALSE));
    }
    else
    {
        LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));

        LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -ENETUNREACH));
    }

    //
    // Test specifying the destination (Ipv4).
    //

    inet_aton("1.1.1.1", &DestinationIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, NULL, -1);

    Request.msg.rtm_family = AF_INET;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckResult(SocketNetlinkRouteGetRouteBestRouteCheckResponse(Socket, TRUE));

    //
    // Test specifying the destination (Ipv6).
    //
    if (IsIpV6Configured)
    {
        memset(&Request, 0, sizeof(Request));
        Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
        Request.nlh.nlmsg_type = RTM_GETROUTE;
        Request.nlh.nlmsg_seq = 0x4567;
        Request.nlh.nlmsg_flags = NLM_F_REQUEST;
        Request.msg.rtm_family = AF_INET6;
        inet_pton(AF_INET6, "12::", &GatewayIpv6);
        LxtCheckErrnoZeroSuccess(
            SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_DST, &DestinationIpv6, sizeof(DestinationIpv6)));

        LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
        LxtCheckResult(SocketNetlinkRouteGetRouteBestRouteCheckResponse(Socket, TRUE));
    }

    //
    // Test specifying the destination and gateway (Ipv4).
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_GETROUTE;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST;
    Request.msg.rtm_family = AF_INET;
    inet_aton("1.1.1.1", &DestinationIpv4);
    inet_aton("1.1.1.2", &GatewayIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, &GatewayIpv4, -1);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckResult(SocketNetlinkRouteGetRouteBestRouteCheckResponse(Socket, TRUE));

    //
    // Test specifying the destination and gateway (Ipv6).
    //

    if (IsIpV6Configured)
    {
        memset(&Request, 0, sizeof(Request));
        Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
        Request.nlh.nlmsg_type = RTM_GETROUTE;
        Request.nlh.nlmsg_seq = 0x4567;
        Request.nlh.nlmsg_flags = NLM_F_REQUEST;
        Request.msg.rtm_family = AF_INET6;
        inet_pton(AF_INET6, "12::", &DestinationIpv6);
        inet_pton(AF_INET6, "13::", &GatewayIpv6);
        LxtCheckErrnoZeroSuccess(
            SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_DST, &DestinationIpv6, sizeof(DestinationIpv6)));

        LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_GATEWAY, &GatewayIpv6, sizeof(GatewayIpv6)));

        LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
        LxtCheckResult(SocketNetlinkRouteGetRouteBestRouteCheckResponse(Socket, TRUE));
    }

    //
    // Test specifying the destination, gateway and interface index (Ipv4).
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_GETROUTE;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST;
    Request.msg.rtm_family = AF_INET;
    inet_aton("127.0.0.1", &DestinationIpv4);
    inet_aton("1.1.1.2", &GatewayIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, &GatewayIpv4, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckResult(SocketNetlinkRouteGetRouteBestRouteCheckResponse(Socket, FALSE));

    //
    // Test specifying the destination, gateway and interface index (Ipv6).
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_GETROUTE;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST;
    Request.msg.rtm_family = AF_INET6;
    inet_pton(AF_INET6, "::1", &DestinationIpv6);
    inet_pton(AF_INET6, "13::", &GatewayIpv6);
    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_DST, &DestinationIpv6, sizeof(DestinationIpv6)));

    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_GATEWAY, &GatewayIpv6, sizeof(GatewayIpv6)));

    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_OIF, &LoopbackIndex, sizeof(LoopbackIndex)));

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckResult(SocketNetlinkRouteGetRouteBestRouteCheckResponse(Socket, FALSE));

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SocketNetlinkRouteGetRouteBestRouteCheckResponse(int Socket, int CheckGateway)

/*++

Routine Description:

    This routine tests the response to the NETLINK_ROUTE protocol's
    RTM_GETROUTE message's get best route request.

Arguments:

    Socket - Supplies the socket to read the response from.

    CheckGateway - Supplies a boolean indicating whether to check for the
        presence of the RTA_GATEWAY attribute.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct rtattr* Attribute;
    char AttributeDump[ATTRIBUTE_DUMP_BUFFER_SIZE];
    int AttributeSeenDst;
    int AttributeSeenGateway;
    int AttributeSeenOif;
    int FoundDone;
    struct nlmsghdr* Header;
    char ReceiveBuffer[5000];
    int ReceiveResult;
    int RemainingLength;
    int Result;
    struct rtmsg* RtMsg;

    AttributeSeenDst = 0;
    AttributeSeenGateway = 0;
    AttributeSeenOif = 0;
    FoundDone = 0;
    LxtCheckErrno(ReceiveResult = recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0, NULL, 0));

    Header = (struct nlmsghdr*)ReceiveBuffer;
    RtMsg = (struct rtmsg*)NLMSG_DATA(Header);
    LxtLogInfo(
        "rtmsg: rtm_family %d rtm_dst_len %d rtm_src_len %d "
        "rtm_tos %d rtm_table %d rtm_protocol %d "
        "rtm_scope %d rtm_type %d rtm_flags %d",
        RtMsg->rtm_family,
        RtMsg->rtm_dst_len,
        RtMsg->rtm_src_len,
        RtMsg->rtm_tos,
        RtMsg->rtm_table,
        RtMsg->rtm_protocol,
        RtMsg->rtm_scope,
        RtMsg->rtm_type,
        RtMsg->rtm_flags);

    Attribute = (struct rtattr*)((char*)RtMsg + sizeof(struct rtmsg));
    RemainingLength = Header->nlmsg_len - NLMSG_LENGTH(sizeof(struct rtmsg));
    while (RTA_OK(Attribute, RemainingLength))
    {
        SocketNetlinkRouteDumpAttributeData(AttributeDump, ATTRIBUTE_DUMP_BUFFER_SIZE, Attribute);

        LxtLogInfo("RTATTR type: %2d len: %3d data: %s", Attribute->rta_type, Attribute->rta_len, AttributeDump);

        if (Attribute->rta_type == RTA_DST)
        {
            AttributeSeenDst += 1;
        }
        else if (Attribute->rta_type == RTA_GATEWAY)
        {
            AttributeSeenGateway += 1;
        }
        else if (Attribute->rta_type == RTA_OIF)
        {
            AttributeSeenOif += 1;
        }

        Attribute = RTA_NEXT(Attribute, RemainingLength);
    }

    LxtCheckTrue(AttributeSeenDst > 0);
    if (CheckGateway != FALSE)
    {
        LxtCheckTrue(AttributeSeenGateway > 0);
    }

    LxtCheckTrue(AttributeSeenOif > 0);
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int SocketNetlinkRouteGetRouteDump(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the NETLINK_ROUTE protocol's RTM_GETROUTE message's
    dump request.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct rtattr* Attribute;
    char AttributeDump[ATTRIBUTE_DUMP_BUFFER_SIZE];
    int AttributeSeenDst;
    int AttributeSeenGateway;
    int AttributeSeenOif;
    int AttributeSeenPriority;
    struct sockaddr_nl BindAddress;
    struct nlmsgerr* Error;
    int FoundDone;
    struct nlmsghdr* Header;
    int Index;
    char ReceiveBuffer[5000];
    int ReceiveResult;
    int RemainingLength;
    int Result;
    struct rtmsg* RtMsg;
    int SendResult;
    int Socket;

    struct
    {
        struct nlmsghdr nlh;
        struct rtmsg msg;
    } Request;

    AttributeSeenDst = 0;
    AttributeSeenGateway = 0;
    AttributeSeenOif = 0;
    AttributeSeenPriority = 0;

    //
    // Create and bind socket. Create a RTM_GETROUTE request.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = sizeof(Request);
    Request.nlh.nlmsg_type = RTM_GETROUTE;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_UNSPEC;

    //
    // Test flags. Only passing the NLM_F_ROOT and/or NLM_F_MATCH flag(s)
    // result in no response.
    //

    Request.nlh.nlmsg_flags = NLM_F_ROOT;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = NLM_F_MATCH;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = NLM_F_ROOT | NLM_F_MATCH;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Test flags. Passing 0 flags or invalid flags result in no response.
    //

    Request.nlh.nlmsg_flags = 0;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = 0x40;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Test flags. NLM_F_REQUEST and at least one of NLM_F_ROOT/NLM_F_MATCH
    // results in the correct response.
    // For the response, verify that at least one interface is present (loopback),
    // and also verify the presence of several attributes.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    FoundDone = 0;
    for (;;)
    {
        LxtCheckErrno(ReceiveResult = recvfrom(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0, NULL, 0));

        Header = (struct nlmsghdr*)ReceiveBuffer;
        while (NLMSG_OK(Header, ReceiveResult))
        {
            if (Header->nlmsg_type == NLMSG_DONE)
            {
                FoundDone = 1;
                break;
            }

            RtMsg = (struct rtmsg*)NLMSG_DATA(Header);
            LxtLogInfo(
                "rtmsg: rtm_family %d rtm_dst_len %d rtm_src_len %d "
                "rtm_tos %d rtm_table %d rtm_protocol %d "
                "rtm_scope %d rtm_type %d rtm_flags %d",
                RtMsg->rtm_family,
                RtMsg->rtm_dst_len,
                RtMsg->rtm_src_len,
                RtMsg->rtm_tos,
                RtMsg->rtm_table,
                RtMsg->rtm_protocol,
                RtMsg->rtm_scope,
                RtMsg->rtm_type,
                RtMsg->rtm_flags);

            Attribute = (struct rtattr*)((char*)RtMsg + sizeof(struct rtmsg));

            RemainingLength = Header->nlmsg_len - NLMSG_LENGTH(sizeof(struct rtmsg));

            while (RTA_OK(Attribute, RemainingLength))
            {
                SocketNetlinkRouteDumpAttributeData(AttributeDump, ATTRIBUTE_DUMP_BUFFER_SIZE, Attribute);

                LxtLogInfo("RTATTR type: %2d len: %3d data: %s", Attribute->rta_type, Attribute->rta_len, AttributeDump);

                if (Attribute->rta_type == RTA_DST)
                {
                    AttributeSeenDst += 1;
                }
                else if (Attribute->rta_type == RTA_GATEWAY)
                {
                    AttributeSeenGateway += 1;
                }
                else if (Attribute->rta_type == RTA_OIF)
                {
                    AttributeSeenOif += 1;
                }
                else if (Attribute->rta_type == RTA_PRIORITY)
                {
                    AttributeSeenPriority += 1;
                }

                Attribute = RTA_NEXT(Attribute, RemainingLength);
            }

            Header = NLMSG_NEXT(Header, ReceiveResult);
        }

        if (FoundDone == 1)
        {
            break;
        }
    }

    LxtCheckTrue(AttributeSeenDst > 0);
    LxtCheckTrue(AttributeSeenGateway > 0);
    LxtCheckTrue(AttributeSeenOif > 0);
    LxtCheckTrue(AttributeSeenPriority > 0);

    //
    // Check that sending is still successful even if the
    // receive buffer has overflown.
    //

    for (Index = 0; Index < 1000; Index++)
    {
        LxtCheckErrno(SendResult = send(Socket, &Request, sizeof(Request), 0));
        LxtCheckEqual(SendResult, sizeof(Request), "%d");
    }

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SocketNetlinkGetLoopbackIndex(int* LoopbackIndex)

/*++

Routine Description:

    This routine obtains the interface index of the loopback adapter.

Arguments:

    LoopbackIndex - Supplies a pointer to an integer that receives the
        interface index of the loopback adapter.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct ifreq InterfaceRequest;
    int Result;
    int Socket;

    LxtCheckErrno(Socket = socket(AF_INET, SOCK_STREAM, 0));
    memset(&InterfaceRequest, 0, sizeof(InterfaceRequest));
    strncpy(InterfaceRequest.ifr_name, SOCKET_LOOPBACK_IF_NAME, sizeof(InterfaceRequest.ifr_name) - 1);

    LxtCheckErrnoZeroSuccess(ioctl(Socket, SIOCGIFINDEX, &InterfaceRequest));
    *LoopbackIndex = InterfaceRequest.ifr_ifindex;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SocketNetlinkCheckResponseError(void* ReceiveBuffer, int ReceiveResult, int Error)

/*++

Routine Description:

    This routine checks the Netlink error message response.

Arguments:

    ReceiveBuffer - Supplies a pointer to the error message.

    ReceiveResult - Supplies the total size of the error message.

    Error - Supplies the error number to check for in the error message.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct nlmsgerr* ErrorMessage;
    struct nlmsghdr* ReceiveHeader;
    int Result;

    ReceiveHeader = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckTrue(NLMSG_OK(ReceiveHeader, ReceiveResult));
    LxtCheckEqual(ReceiveHeader->nlmsg_type, NLMSG_ERROR, "%d");
    ErrorMessage = (struct nlmsgerr*)NLMSG_DATA(ReceiveHeader);
    LxtCheckEqual(ErrorMessage->error, Error, "%d");
    Result = 0;

ErrorExit:
    return Result;
}

int SocketNetlinkRouteAddAttribute(struct nlmsghdr* Msghdr, int MessageSize, int AttributeType, void* AttributeData, int AttributeSize)

/*++

Routine Description:

    This routine adds a RT attribute to the Netlink message.

Arguments:

    Msghdr - Supplies the message header.

    MessageSize - Supplies the total possible size of the message.

    AttributeType - Supplies the RT attribute type.

    AttributeData - Supplies the RT attribute data.

    AttributeSize - Supplies the RT attribute size.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Length;
    struct rtattr* Attribute;

    Length = RTA_LENGTH(AttributeSize);
    if (NLMSG_ALIGN(Msghdr->nlmsg_len) + RTA_ALIGN(Length) > MessageSize)
    {
        LxtLogError("Adding RT attribute, message size overflowed: %d %d %d", Msghdr->nlmsg_len, Length, MessageSize);

        return -1;
    }

    Attribute = NLMSG_TAIL(Msghdr);
    Attribute->rta_type = AttributeType;
    Attribute->rta_len = Length;
    memcpy(RTA_DATA(Attribute), AttributeData, AttributeSize);
    Msghdr->nlmsg_len = NLMSG_ALIGN(Msghdr->nlmsg_len) + RTA_ALIGN(Length);
    return 0;
}

void SocketNetlinkRouteAddAddressAttributes(struct nlmsghdr* Msghdr, int MessageSize, struct in_addr* AddressIpv4)

/*++

Routine Description:

    This routine appends the RTM_*ADDR message RT attributes to the message.

Arguments:

    Msghdr - Supplies the message header.

    MessageSize - Supplies the total possible size of the message.

    AddressIpv4 - Supplies a pointer to the IPv4 address RT attribute.

Return Value:

    None.

--*/

{

    int Result;

    if (AddressIpv4 != NULL)
    {
        LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(Msghdr, MessageSize, IFA_ADDRESS, AddressIpv4, sizeof(*AddressIpv4)));

        LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(Msghdr, MessageSize, IFA_LOCAL, AddressIpv4, sizeof(*AddressIpv4)));
    }

ErrorExit:
    return;
}

void SocketNetlinkRouteAddRouteAttributes(struct nlmsghdr* Msghdr, int MessageSize, struct in_addr* DestinationIpv4, struct in_addr* GatewayIpv4, int InterfaceIndex)

/*++

Routine Description:

    This routine appends the RTM_*ROUTE message RT attributes to the message.

Arguments:

    Msghdr - Supplies the message header.

    MessageSize - Supplies the total possible size of the message.

    DestinationIpv4 - Supplies a pointer to the destination IPv4 address RT attribute.

    GatewayIpv4 - Supplies a pointer to the gateway IPv4 address RT attribute.

    InterfaceIndex - Supplies the interface index RT attribute.

Return Value:

    None.

--*/

{

    int Result;

    if (DestinationIpv4 != NULL)
    {
        LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(Msghdr, MessageSize, RTA_DST, DestinationIpv4, sizeof(*DestinationIpv4)));
    }

    if (GatewayIpv4 != NULL)
    {
        LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(Msghdr, MessageSize, RTA_GATEWAY, GatewayIpv4, sizeof(*GatewayIpv4)));
    }

    if (InterfaceIndex > 0)
    {
        LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(Msghdr, MessageSize, RTA_OIF, &InterfaceIndex, sizeof(InterfaceIndex)));
    }

ErrorExit:
    return;
}

int SocketNetlinkRouteNewDelAddress(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the NETLINK_ROUTE protocol's
    RTM_NEWADDR and RTM_DELADDR messages.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct in_addr AddressIpv4;
    struct in6_addr AddressIpv6;
    socklen_t AddressLength;
    struct sockaddr_nl BindAddress;
    struct nlmsgerr* Error;
    struct in_addr GatewayIpv4;
    struct in6_addr GatewayIpv6;
    int LoopbackIndex;
    char ReceiveBuffer[5000];
    struct nlmsghdr* ReceiveHeader;
    int ReceiveResult;
    struct
    {
        struct nlmsghdr nlh;
        struct ifaddrmsg msg;
        char attr[200];
    } Request;
    int Result;
    int Retries;
    int Socket;

    //
    // Create and bind socket. Create a RTM_GETROUTE request.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    Request.nlh.nlmsg_type = RTM_NEWADDR;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.ifa_family = AF_UNSPEC;

    //
    // Get the interface index of the loopback adapter. All the tests
    // below operate on the loopback adapter.
    //

    LxtCheckErrnoZeroSuccess(SocketNetlinkGetLoopbackIndex(&LoopbackIndex));
    LxtCheckTrue(LoopbackIndex > 0);

    //
    // Test flags. Passing in (invalid) flags for RTM_GET* results in no response.
    //

    Request.nlh.nlmsg_flags = NLM_F_ROOT;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = NLM_F_MATCH;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = NLM_F_ROOT | NLM_F_MATCH;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Test invalid flags with NLM_F_ACK.
    //

    Request.nlh.nlmsg_flags = NLM_F_ROOT | NLM_F_MATCH | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    //
    // Test flags. Passing 0 flags or invalid flags result in no response.
    //

    Request.nlh.nlmsg_flags = 0;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = 0x40;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Add an ip address 2.1.1.1/30.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    Request.nlh.nlmsg_type = RTM_NEWADDR;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.ifa_prefixlen = 30;
    Request.msg.ifa_index = LoopbackIndex;
    inet_aton("2.1.1.1", &AddressIpv4);

    //
    // Test specifying an invalid address family.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST;
    Request.msg.ifa_family = AF_UNSPEC;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EOPNOTSUPP));

    //
    // Test not specifying the ip address, which should fail.
    //

    Request.msg.ifa_family = AF_INET;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_EXCL | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EINVAL));

    //
    // Basically any flag allows you to create.
    // Windows can take up to 10 seconds before sending the WNF notification that lxcore
    // uses to update its internal network interface and address cache. Before the cache
    // is updated, this create operation will fail, so wait for 20 seconds for the
    // operation to succeed.
    //

    SocketNetlinkRouteAddAddressAttributes(&Request.nlh, sizeof(Request), &AddressIpv4);

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_EXCL | NLM_F_ACK;
    LxtCheckResult(SocketNetlinkSendAndWaitForExpectedError(Socket, &Request, sizeof(Request), 0));

    //
    // Test NLM_F_REPLACE, which should succeed.
    // Windows can take up to 10 seconds before sending the WNF notification that lxcore
    // uses to update its internal network interface and address cache. Before the cache
    // is updated, this replace operation will fail, so wait for 20 seconds for the
    // operation to succeed.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_ACK;
    LxtCheckResult(SocketNetlinkSendAndWaitForExpectedError(Socket, &Request, sizeof(Request), 0));

    //
    // Test NLM_F_EXCL, which should fail since it prevents existing addresses
    // from being changed.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_EXCL | NLM_F_ACK;
    LxtCheckResult(SocketNetlinkSendAndWaitForExpectedError(Socket, &Request, sizeof(Request), -EEXIST));

    //
    // Even if NLM_F_REPLACE is added to NLM_F_EXCL, it still fails.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_EXCL | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EEXIST));

    //
    // NLM_F_CREATE without NLM_F_REPLACE fails.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EEXIST));

    //
    // NLM_F_CREATE with NLM_F_REPLACE succeeds.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    //
    // Test giving an invalid interface index, which should fail.
    //

    Request.msg.ifa_index = 90000;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -ENODEV));

    //
    // Add an ip address 2.1.1.2/32.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    Request.nlh.nlmsg_type = RTM_NEWADDR;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.ifa_family = AF_INET;
    Request.msg.ifa_prefixlen = 32;
    Request.msg.ifa_index = LoopbackIndex;
    inet_aton("2.1.1.2", &AddressIpv4);
    SocketNetlinkRouteAddAddressAttributes(&Request.nlh, sizeof(Request), &AddressIpv4);

    //
    // Only setting the NLM_F_REQUEST flag allows creating a new ip address.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    //
    // Add an ip address 11::/31.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    Request.nlh.nlmsg_type = RTM_NEWADDR;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.ifa_family = AF_INET6;
    Request.msg.ifa_prefixlen = 31;
    Request.msg.ifa_index = LoopbackIndex;
    inet_pton(AF_INET6, "11::", &AddressIpv6);
    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), IFA_ADDRESS, &AddressIpv6, sizeof(AddressIpv6)));

    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), IFA_LOCAL, &AddressIpv6, sizeof(AddressIpv6)));

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    //
    // Delete 2.1.1.1/30.
    //

    usleep(1000 * 40);
    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    Request.nlh.nlmsg_type = RTM_DELADDR;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.ifa_family = AF_INET;
    Request.msg.ifa_prefixlen = 30;
    Request.msg.ifa_index = LoopbackIndex;
    inet_aton("2.1.1.1", &AddressIpv4);
    SocketNetlinkRouteAddAddressAttributes(&Request.nlh, sizeof(Request), &AddressIpv4);

    //
    // Passing 0 flags results in no response.
    //

    Request.nlh.nlmsg_flags = 0;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Need at least NLM_F_REQUEST.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    //
    // Try deleting again, which should fail.
    //

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EADDRNOTAVAIL));

    //
    // Delete 2.1.1.2/32.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    Request.nlh.nlmsg_type = RTM_DELADDR;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.ifa_family = AF_INET;
    Request.msg.ifa_prefixlen = 32;
    Request.msg.ifa_index = LoopbackIndex;
    inet_aton("2.1.1.2", &AddressIpv4);
    SocketNetlinkRouteAddAddressAttributes(&Request.nlh, sizeof(Request), &AddressIpv4);

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_EXCL | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    //
    // Test giving an invalid interface index, which should fail.
    //

    Request.msg.ifa_index = 90000;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -ENODEV));

    //
    // Delete ip address 11::/31.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    Request.nlh.nlmsg_type = RTM_DELADDR;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.ifa_family = AF_INET6;
    Request.msg.ifa_prefixlen = 31;
    Request.msg.ifa_index = LoopbackIndex;
    inet_pton(AF_INET6, "11::", &AddressIpv6);

    //
    // Test not specifying the ip address, which should fail.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EINVAL));

    //
    // Now, this should succeed.
    //

    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), IFA_ADDRESS, &AddressIpv6, sizeof(AddressIpv6)));

    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), IFA_LOCAL, &AddressIpv6, sizeof(AddressIpv6)));

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SocketNetlinkRouteNewDelRoute(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests the NETLINK_ROUTE protocol's
    RTM_NEWROUTE and RTM_DELROUTE messages.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct sockaddr_nl BindAddress;
    struct in_addr DestinationIpv4;
    struct in6_addr DestinationIpv6;
    struct in_addr GatewayIpv4;
    struct in6_addr GatewayIpv6;
    int LoopbackIndex;
    char ReceiveBuffer[5000];
    int ReceiveResult;
    struct
    {
        struct nlmsghdr nlh;
        struct rtmsg msg;
        char attr[200];
    } Request;
    int Result;
    int Socket;

    //
    // Create and bind socket. Create a RTM_GETROUTE request.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_NEWROUTE;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_UNSPEC;

    //
    // Get the interface index of the loopback adapter. All the tests
    // below operate on the loopback adapter.
    //

    LxtCheckErrnoZeroSuccess(SocketNetlinkGetLoopbackIndex(&LoopbackIndex));
    LxtCheckTrue(LoopbackIndex > 0);

    //
    // Test flags. Passing in (invalid) flags for RTM_GET* results in no response.
    //

    Request.nlh.nlmsg_flags = NLM_F_ROOT;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = NLM_F_MATCH;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = NLM_F_ROOT | NLM_F_MATCH;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Test invalid flags with NLM_F_ACK.
    //

    Request.nlh.nlmsg_flags = NLM_F_ROOT | NLM_F_MATCH | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    //
    // Test flags. Passing 0 flags or invalid flags result in no response.
    //

    Request.nlh.nlmsg_flags = 0;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.nlh.nlmsg_flags = 0x40;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Add a routing entry to lo with destination 1.1.1.1 and on-link gateway.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_NEWROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_protocol = RTPROT_BOOT;
    Request.msg.rtm_scope = RT_SCOPE_LINK;
    Request.msg.rtm_type = RTN_UNICAST;
    inet_aton("1.1.1.1", &DestinationIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, NULL, -1);

    //
    // Test specifying an invalid address family.
    //

    Request.msg.rtm_family = AF_UNSPEC;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EOPNOTSUPP));

    //
    // Only send the request with the destination and no interface index.
    // The request should fail with ENODEV, because no interface index was specified.
    //

    Request.msg.rtm_family = AF_INET;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -ENODEV));

    //
    // Send the request with only NLM_F_REQUEST. The request should fail with ENOENT,
    // because destination 1.1.1.1 does not exist and NLM_F_CREATE was not specified.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST;
    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_OIF, &LoopbackIndex, sizeof(LoopbackIndex)));

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -ENOENT));

    //
    // Now add the NLM_F_REPLACE flag, and the request should still fail.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -ENOENT));

    //
    // Now add the NLM_F_CREATE flag, and the request should succeed.
    // Since the NLM_F_ACK flag was not specified, there should be no response,
    // even though the operation succeeded.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Sending again gets an error message EEXIST.
    //
    // N.B. The sleep is to wait for the lxcore internal cache to be updated
    //      with the latest routing info from NETIO.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE;
    LxtCheckResult(SocketNetlinkSendAndWaitForExpectedError(Socket, &Request, sizeof(Request), -EEXIST));

    //
    // Sending again with only NLM_F_REQUEST still gets an error message EEXIST.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EEXIST));

    //
    // Sending again with NLM_F_REQUEST and NLM_F_EXCL still gets an error message EEXIST.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_EXCL;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EEXIST));

    //
    // Sending again after adding NLM_F_REPLACE still gets an error message EEXIST.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_EXCL | NLM_F_REPLACE;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EEXIST));

    //
    // Sending again with NLM_F_REPLACE is successful.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_APPEND | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    //
    // Add a routing entry to lo with destination 1.1.1.2 and on-link gateway.
    // Send the request with various flags. The request should succeed.
    //

    usleep(1000 * 60);
    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_NEWROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_protocol = RTPROT_BOOT;
    Request.msg.rtm_scope = RT_SCOPE_LINK;
    Request.msg.rtm_type = RTN_UNICAST;
    inet_aton("1.1.1.2", &DestinationIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, NULL, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Sending again gets an error message EEXIST.
    //

    usleep(1000 * 60);
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckEqual(ReceiveResult, Request.nlh.nlmsg_len + NLMSG_LENGTH(sizeof(int)), "%d");
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EEXIST));

    //
    // Sending again with the following flags still gets an error message EEXIST.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_EXCL | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckEqual(ReceiveResult, Request.nlh.nlmsg_len + NLMSG_LENGTH(sizeof(int)), "%d");
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EEXIST));

    //
    // Use NLM_F_REPLACE to change the gateway from on-link to 1.1.1.1.
    // Note that including the NLM_F_EXCL flag fails the operation.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL;
    inet_aton("1.1.1.1", &GatewayIpv4);
    Request.msg.rtm_scope = RT_SCOPE_UNIVERSE;
    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_GATEWAY, &GatewayIpv4, sizeof(GatewayIpv4)));

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckEqual(ReceiveResult, Request.nlh.nlmsg_len + NLMSG_LENGTH(sizeof(int)), "%d");
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -EEXIST));

    //
    // Now remove the NLM_F_EXCL flag, and the operation should succeed.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_ACK;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    //
    // Add a routing entry to lo with destination 1.1.1.3 and gateway 1.1.1.1.
    // Send the request with various flags. The request should succeed.
    //

    usleep(1000 * 60);
    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_NEWROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_protocol = RTPROT_BOOT;
    Request.msg.rtm_scope = RT_SCOPE_UNIVERSE;
    Request.msg.rtm_type = RTN_UNICAST;
    inet_aton("1.1.1.3", &DestinationIpv4);
    inet_aton("1.1.1.1", &GatewayIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, &GatewayIpv4, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Change the gateway from 1.1.1.1 to on-link.
    // Note that invalid flags are silently dropped.
    //

    usleep(1000 * 60);
    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_NEWROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | 0xF800;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_protocol = RTPROT_BOOT;
    Request.msg.rtm_scope = RT_SCOPE_LINK;
    Request.msg.rtm_type = RTN_UNICAST;
    inet_aton("1.1.1.3", &DestinationIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, NULL, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Add two more routing entries: destination 1.1.2 and 1.3,
    // with 1.1.1.1 as their gateway.
    //

    usleep(1000 * 60);
    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_NEWROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_ACK;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_protocol = RTPROT_BOOT;
    Request.msg.rtm_scope = RT_SCOPE_UNIVERSE;
    Request.msg.rtm_type = RTN_UNICAST;
    inet_aton("1.1.2", &DestinationIpv4);
    inet_aton("1.1.1.1", &GatewayIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, &GatewayIpv4, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    usleep(1000 * 60);
    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_NEWROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_protocol = RTPROT_BOOT;
    Request.msg.rtm_scope = RT_SCOPE_UNIVERSE;
    Request.msg.rtm_type = RTN_UNICAST;
    inet_aton("1.3", &DestinationIpv4);
    inet_aton("1.1.1.1", &GatewayIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, &GatewayIpv4, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Add three more routing entries: destination 1.1.1.0,
    // with 1.1.1.1 as their gateway, and prefix lengths 30, 31 and 32.
    //

    usleep(1000 * 60);
    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_NEWROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 30;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_protocol = RTPROT_BOOT;
    Request.msg.rtm_scope = RT_SCOPE_UNIVERSE;
    Request.msg.rtm_type = RTN_UNICAST;
    inet_aton("1.1.1.0", &DestinationIpv4);
    inet_aton("1.1.1.1", &GatewayIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, &GatewayIpv4, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    usleep(1000 * 60);
    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_NEWROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 31;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_protocol = RTPROT_BOOT;
    Request.msg.rtm_scope = RT_SCOPE_UNIVERSE;
    Request.msg.rtm_type = RTN_UNICAST;
    inet_aton("1.1.1.0", &DestinationIpv4);
    inet_aton("1.1.1.1", &GatewayIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, &GatewayIpv4, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    usleep(1000 * 60);
    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_NEWROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_protocol = RTPROT_BOOT;
    Request.msg.rtm_scope = RT_SCOPE_UNIVERSE;
    Request.msg.rtm_type = RTN_UNICAST;
    inet_aton("1.1.1.0", &DestinationIpv4);
    inet_aton("1.1.1.1", &GatewayIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, &GatewayIpv4, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    //
    // Test giving an invalid interface index, which should fail.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_NEWROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_protocol = RTPROT_BOOT;
    Request.msg.rtm_scope = RT_SCOPE_UNIVERSE;
    Request.msg.rtm_type = RTN_UNICAST;
    inet_aton("1.1.1.10", &DestinationIpv4);
    inet_aton("1.1.1.1", &GatewayIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, &GatewayIpv4, 90000);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -ENETUNREACH));

    //
    // Add 2 Ipv6 entries.
    //

    usleep(1000 * 60);
    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_NEWROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET6;
    Request.msg.rtm_dst_len = 31;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_protocol = RTPROT_BOOT;
    Request.msg.rtm_scope = RT_SCOPE_UNIVERSE;
    Request.msg.rtm_type = RTN_UNICAST;
    inet_pton(AF_INET6, "3ffa::", &DestinationIpv6);
    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_DST, &DestinationIpv6, sizeof(DestinationIpv6)));

    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_OIF, &LoopbackIndex, sizeof(LoopbackIndex)));

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    usleep(1000 * 60);
    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_NEWROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET6;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_protocol = RTPROT_BOOT;
    Request.msg.rtm_scope = RT_SCOPE_UNIVERSE;
    Request.msg.rtm_type = RTN_UNICAST;
    inet_pton(AF_INET6, "3ffa::", &DestinationIpv6);
    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_DST, &DestinationIpv6, sizeof(DestinationIpv6)));

    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_OIF, &LoopbackIndex, sizeof(LoopbackIndex)));

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    //
    // Now test RTM_DELROUTE!
    // Now, we have the following routing entries for the loopback adapter:
    // Destination    Gateway
    // 1.1.1.0/30     1.1.1.1
    // 1.1.1.0/31     1.1.1.1
    // 1.1.1.0/32     1.1.1.1
    // 1.1.1.1        on-link (zero)
    // 1.1.1.2        1.1.1.1
    // 1.1.1.3        on-link (zero)
    // 1.1.2          1.1.1.1
    // 1.3            1.1.1.1
    // === IPv6 ===
    // 3ffa::/31      on-link
    // 3ffa::/32      on-link
    //

    //
    // Try to delete the routing entry with destination 1.1.1.1 and
    // gateway 1.2.3.4, which does not exist, returning ESRCH.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_DELROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL | 0x4321;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_scope = RT_SCOPE_NOWHERE;
    inet_aton("1.1.1.1", &DestinationIpv4);
    inet_aton("1.2.3.4", &GatewayIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, &GatewayIpv4, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -ESRCH));

    //
    // Passing 0 flags results in no response.
    //

    Request.nlh.nlmsg_flags = 0;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Try to delete the routing entry with destination 1.1.1.1 and
    // gateway 0.0.0.0, which should succeed.
    // Note that as long as the NLM_F_REQUEST flag is present, it does
    // not matter what the rest of the flags are.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_DELROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL | 0x4321;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_scope = RT_SCOPE_NOWHERE;
    inet_aton("1.1.1.1", &DestinationIpv4);
    inet_aton("0.0.0.0", &GatewayIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, &GatewayIpv4, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Try to delete the routing entry with destination 1.1.1.2 and
    // gateway 1.2.3.4, which does not exist, returning ESRCH.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_DELROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL | 0x4321;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_scope = RT_SCOPE_NOWHERE;
    inet_aton("1.1.1.2", &DestinationIpv4);
    inet_aton("1.2.3.4", &GatewayIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, &GatewayIpv4, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -ESRCH));

    //
    // Try to delete the routing entry with destination 1.1.1.2 and
    // gateway 0.0.0.0, which should succeed.
    // Note that even though the gateway is 1.1.1.1, specifying
    // 0.0.0.0 in the request specifies a wildcard gateway.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_DELROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL | 0x4321;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_scope = RT_SCOPE_NOWHERE;
    inet_aton("1.1.1.2", &DestinationIpv4);
    inet_aton("0.0.0.0", &GatewayIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, &GatewayIpv4, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Test giving an invalid interface index, which should fail.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_DELROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK | 0x4321;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_scope = RT_SCOPE_NOWHERE;
    inet_aton("1.1.1.3", &DestinationIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, NULL, 90000);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -ESRCH));

    //
    // Try to delete the routing entry with destination 1.1.1.3 and
    // no gateway specified, which should succeed.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_DELROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK | 0x4321;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_scope = RT_SCOPE_NOWHERE;
    inet_aton("1.1.1.3", &DestinationIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, NULL, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    //
    // Try to delete the routing entry with destination 1.1.2 and
    // no gateway specified, which should succeed.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_DELROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK | 0x4321;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_scope = RT_SCOPE_NOWHERE;
    inet_aton("1.1.2", &DestinationIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, NULL, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

    //
    // Try to delete the routing entry with destination 1.3 and
    // gateway 1.1.1.1 specified, which should succeed.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_DELROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL | 0x4321;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_scope = RT_SCOPE_NOWHERE;
    inet_aton("1.3", &DestinationIpv4);
    inet_aton("1.1.1.1", &GatewayIpv4);
    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_GATEWAY, &GatewayIpv4, sizeof(GatewayIpv4)));

    //
    // First try sending incomplete information (only sending the gateway
    // and nothing else), which should fail.
    //

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -ESRCH));

    //
    // Now try the actual delete.
    //

    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_DST, &DestinationIpv4, sizeof(DestinationIpv4)));

    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_OIF, &LoopbackIndex, sizeof(LoopbackIndex)));

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Try to delete the routing entries with destination 1.1.1.0 and prefix
    // lengths 29, 30, 31 and 32. Only 29 should fail and the rest should succeed.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_DELROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL | 0x4321;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET;
    Request.msg.rtm_dst_len = 29;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_scope = RT_SCOPE_NOWHERE;
    inet_aton("1.1.1.0", &DestinationIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, NULL, LoopbackIndex);

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, -ESRCH));

    Request.msg.rtm_dst_len = 30;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.msg.rtm_dst_len = 31;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    Request.msg.rtm_dst_len = 32;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Try to delete the routing entry with destination 3ffa:: and prefix length 31.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_DELROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL | 0x4321;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET6;
    Request.msg.rtm_dst_len = 31;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_scope = RT_SCOPE_NOWHERE;
    inet_pton(AF_INET6, "3ffa::", &DestinationIpv6);
    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_DST, &DestinationIpv6, sizeof(DestinationIpv6)));

    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_OIF, &LoopbackIndex, sizeof(LoopbackIndex)));

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrnoFailure(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), MSG_DONTWAIT), EAGAIN);

    //
    // Try to delete the routing entry with destination 3ffa:: and prefix length 32.
    //

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_DELROUTE;
    Request.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK | 0x4321;
    Request.nlh.nlmsg_seq = 0x4567;
    Request.msg.rtm_family = AF_INET6;
    Request.msg.rtm_dst_len = 32;
    Request.msg.rtm_table = RT_TABLE_MAIN;
    Request.msg.rtm_scope = RT_SCOPE_NOWHERE;
    inet_pton(AF_INET6, "3ffa::", &DestinationIpv6);
    inet_pton(AF_INET6, "0::", &GatewayIpv6);
    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_DST, &DestinationIpv6, sizeof(DestinationIpv6)));

    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_GATEWAY, &GatewayIpv6, sizeof(GatewayIpv6)));

    LxtCheckErrnoZeroSuccess(SocketNetlinkRouteAddAttribute(&Request.nlh, sizeof(Request), RTA_OIF, &LoopbackIndex, sizeof(LoopbackIndex)));

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckResult(SocketNetlinkCheckResponseError(ReceiveBuffer, ReceiveResult, 0));

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SocketNetlinkSendAndWaitForExpectedError(int Socket, void* Request, int RequestSize, int ExpectedError)

/*++

Routine Description:

    This routine repeatedly sends a request and receives the Netlink error
    response until the Netlink error value matches the passed in expected value.

Arguments:

    Socket - Supplies the socket to send and receive the response from.

    Request -  Supplies the Netlink request to send.

    RequestSize - Supplies the size of the Netlink request.

    ExpectedError - Supplies the Netlink error to check for.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct nlmsgerr* Error;
    char ReceiveBuffer[5000];
    struct nlmsghdr* ReceiveHeader;
    int ReceiveResult;
    int Result;
    int Retries;

    Retries = 0;
    while (Retries < 20)
    {
        Retries += 1;
        LxtCheckErrno(sendto(Socket, Request, RequestSize, 0, NULL, 0));
        LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
        ReceiveHeader = (struct nlmsghdr*)ReceiveBuffer;
        LxtCheckTrue(NLMSG_OK(ReceiveHeader, ReceiveResult));
        LxtCheckEqual(ReceiveHeader->nlmsg_type, NLMSG_ERROR, "%d");
        Error = (struct nlmsgerr*)NLMSG_DATA(ReceiveHeader);
        if (Error->error == ExpectedError)
        {
            break;
        }
        else
        {
            LxtLogInfo("Error is: %d, waiting for it to become %d.", Error->error, ExpectedError);
        }

        usleep(1000 * 1000);
    }

    LxtCheckEqual(Error->error, ExpectedError, "%d");
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int SocketNetlinkSetAndVerifySocketOptionTimeout(int Socket, int SocketOption, int Usec)

/*++

Routine Description:

    This routine sets a timeout socket option and reads it back to verify.

Arguments:

    Socket - Supplies the socket to set the socket option on.

    SocketOption -  Supplies the timeout-related socket option to set.

    Usec - Supplies the number of microseconds for the timeout.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t OptionLength;
    int Result;
    struct timeval Timeout;

    OptionLength = sizeof(Timeout);
    Timeout.tv_sec = 0;
    Timeout.tv_usec = Usec;
    LxtCheckErrno(setsockopt(Socket, SOL_SOCKET, SocketOption, &Timeout, OptionLength));

    memset(&Timeout, 0, sizeof(Timeout));
    LxtCheckErrno(getsockopt(Socket, SOL_SOCKET, SocketOption, &Timeout, &OptionLength));

    LxtCheckEqual(OptionLength, sizeof(Timeout), "%d");
    LxtCheckEqual(Timeout.tv_sec, 0, "%d");
    LxtCheckEqual(Timeout.tv_usec, Usec, "%d");
    Result = LXT_RESULT_SUCCESS;

ErrorExit:
    return Result;
}

int SocketNetlinkSoPasscred(PLXT_ARGS Args)

/*++

Routine Description:

    This routine tests for the SO_PASSCRED socket option.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct sockaddr_nl BindAddress;
    unsigned char ControlBuffer[CMSG_SPACE(sizeof(struct ucred))];
    struct cmsghdr* ControlMessage;
    struct in_addr DestinationIpv4;
    struct iovec IoVector;
    struct msghdr MessageHeader = {0};
    int PassCredentials;
    char ReceiveBuffer[5000];
    int Result;
    int Socket;

    struct
    {
        struct nlmsghdr nlh;
        struct rtmsg msg;
        char attr[200];
    } Request;

    Socket = -1;

    //
    // Create and bind socket. Create a RTM_GETROUTE request.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    AddressLength = sizeof(BindAddress);
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, AddressLength));

    //
    // Enable SO_PASSCRED on the socket.
    //

    PassCredentials = 1;
    LxtCheckErrno(setsockopt(Socket, SOL_SOCKET, SO_PASSCRED, &PassCredentials, sizeof(PassCredentials)));

    memset(&Request, 0, sizeof(Request));
    Request.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    Request.nlh.nlmsg_type = RTM_GETROUTE;
    Request.nlh.nlmsg_seq = 0x4567;

    //
    // NLM_F_REQUEST with no NLM_F_DUMP means "get best route" request.
    //

    Request.nlh.nlmsg_flags = NLM_F_REQUEST;

    //
    // Specify the destination (Ipv4).
    //

    inet_aton("1.1.1.1", &DestinationIpv4);
    SocketNetlinkRouteAddRouteAttributes(&Request.nlh, sizeof(Request), &DestinationIpv4, NULL, -1);

    Request.msg.rtm_family = AF_INET;
    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));

    //
    // Craft the message header to receive the data + ancillary data.
    //

    memset(&MessageHeader, 0, sizeof(MessageHeader));
    MessageHeader.msg_iov = &IoVector;
    MessageHeader.msg_iovlen = 1;
    MessageHeader.msg_control = ControlBuffer;
    MessageHeader.msg_controllen = sizeof(ControlBuffer);
    IoVector.iov_base = ReceiveBuffer;
    IoVector.iov_len = sizeof(ReceiveBuffer);
    memset(ControlBuffer, 0, sizeof(ControlBuffer));
    LxtCheckErrno(Result = recvmsg(Socket, &MessageHeader, 0));
    LxtCheckGreater(Result, 0, "%d");
    LxtCheckEqual(MessageHeader.msg_controllen, sizeof(ControlBuffer), "%d");
    LxtCheckEqual(MessageHeader.msg_flags, 0, "%d");

    //
    // Validate that SMC_CREDENTIALS control message is present and has valid
    // values.
    //

    ControlMessage = SocketGetControlMessage(&MessageHeader, NULL, SOL_SOCKET, SCM_CREDENTIALS);

    LxtCheckNotEqual(ControlMessage, NULL, "%p");
    LxtCheckEqual(ControlMessage->cmsg_len, CMSG_LEN(sizeof(struct ucred)), "%d");

    LxtCheckAncillaryCredentials(ControlMessage, 0, 0, 0);

    //
    // Pass NULL as the control buffer with invalid size.
    //

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    memset(&MessageHeader, 0, sizeof(MessageHeader));
    MessageHeader.msg_iov = &IoVector;
    MessageHeader.msg_iovlen = 1;
    MessageHeader.msg_control = NULL;
    MessageHeader.msg_controllen = sizeof(ControlBuffer);
    IoVector.iov_base = ReceiveBuffer;
    IoVector.iov_len = sizeof(ReceiveBuffer);
    LxtCheckErrno(Result = recvmsg(Socket, &MessageHeader, 0));
    LxtCheckGreater(Result, 0, "%d");
    LxtCheckEqual(MessageHeader.msg_controllen, 0, "%d");

    //
    // Since the control buffer was not big enough (NULL) to hold the control
    // message, proper (truncate) flags should be set.
    //

    LxtCheckEqual(MessageHeader.msg_flags, MSG_CTRUNC, "%d");

    //
    // Pass a control buffer smaller than the control message header.
    //

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    memset(&MessageHeader, 0, sizeof(MessageHeader));
    MessageHeader.msg_iov = &IoVector;
    MessageHeader.msg_iovlen = 1;
    MessageHeader.msg_control = NULL;
    MessageHeader.msg_controllen = sizeof(struct cmsghdr) - 1;
    IoVector.iov_base = ReceiveBuffer;
    IoVector.iov_len = sizeof(ReceiveBuffer);
    LxtCheckErrno(Result = recvmsg(Socket, &MessageHeader, 0));
    LxtCheckGreater(Result, 0, "%d");
    LxtCheckEqual(MessageHeader.msg_controllen, 0, "%d");

    //
    // Since the control buffer was not big enough (NULL) to hold the control
    // message, proper (truncate) flags should be set.
    //

    LxtCheckEqual(MessageHeader.msg_flags, MSG_CTRUNC, "%d");

    //
    // Pass a NULL message header.
    //

    LxtCheckErrno(sendto(Socket, &Request, sizeof(Request), 0, NULL, 0));
    memset(&ReceiveBuffer, 0, sizeof(ReceiveBuffer));
    LxtCheckErrno(Result = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));
    LxtCheckGreater(Result, 0, "%d");

    //
    // Check for general set/get of the SO_PASSCRED socket option.
    //
    // N.B After this routine, the state of the 'SO_PASSCRED' socket option in
    //     the socket is not guaranteed.
    //

    LxtCheckErrno(SocketGetSetBooleanSocketOption(Socket, SOL_SOCKET, SO_PASSCRED, false));

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}
