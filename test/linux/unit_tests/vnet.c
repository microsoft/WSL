/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    vnet.c

Abstract:

    This file is a test of virtual network devices.

--*/

#include "lxtcommon.h"
#include "unittests.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <bits/local_lim.h>
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
#include <linux/net_namespace.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <linux/veth.h>

//
// Globals.
//

#define LXT_NAME "VirtualNetwork"
#define LXT_REQUEST_SEQUENCE 0x4567
#define LXT_IP_ADDRESS1 "172.12.13.113"
#define LXT_IP_ADDRESS2 "172.12.13.114"

//
// Functions.
//

int BindSocketForNetlink(int Socket);

int CreateVirtualBridgeViaIoctl(const char* Name);

int CreateVirtualBridgeViaNetlink(const char* Name);

void CreateVirtualDeviceInfo(const char* Type, const char* Name, const char* DeviceData, int DeviceDataSize, char* Buffer, int* BufferSize);

int CreateVirtualDeviceViaNetlink(int Flags, const char* DeviceData, int DeviceDataSize, struct nlmsghdr** Response);

int CreateVirtualEthernetPairViaNetlink(const char* Name, const char* PeerName);

int DeleteVirtualDeviceViaNetlink(const char* Name);

int DeleteVirtualBridgeViaIoctl(const char* Name);

int GetNetworkInterfaceIndex(const char* Name);

int SetIpAddress(const char* InterfaceName, const struct in_addr* AddressIpv4, char PrefixLength);

int SetRoute(const char* InterfaceName, const struct in_addr* AddressIpv4, int PrefixLength);

int SetVirtualDeviceAttributeViaNetlink(const char* Name, int AttributeType, int AttributeValue, int* Response);

int SetVirtualDeviceFlagViaNetlink(const char* Name, int Flag, bool IsFlagEnabled, int* Response);

int VerifyRouteLinkDoesNotExist(
    const char* InterfaceName,
    /* optional */ int* SocketToUse);

int VerifyRouteLinkExists(
    const char* InterfaceName,
    /* optional */ int* SocketToUse);

//
// Test cases.
//

LXT_VARIATION_HANDLER EmptyDeviceErrorFromNetlink;
LXT_VARIATION_HANDLER PhysicalDeviceNamespace1;
LXT_VARIATION_HANDLER SanityPermissionsCheck;
LXT_VARIATION_HANDLER VirtualBridgeAutoName;
LXT_VARIATION_HANDLER VirtualBridgeFromIoctl1;
LXT_VARIATION_HANDLER VirtualBridgeNamespace1;
LXT_VARIATION_HANDLER VirtualBridgeFromNetlink1;
LXT_VARIATION_HANDLER VirtualEthernetPairFromNetlink1;
LXT_VARIATION_HANDLER VirtualEthernetPairFromNetlink2;
LXT_VARIATION_HANDLER VirtualEthernetPairNamespace1;
LXT_VARIATION_HANDLER VirtualEthernetPairNamespace2;
LXT_VARIATION_HANDLER VirtualEthernetPairNamespace3;
LXT_VARIATION_HANDLER VirtualEthernetPairNamespace4;
LXT_VARIATION_HANDLER VirtualEthernetPairNamespace5;
LXT_VARIATION_HANDLER VirtualEthernetPairConfigure;
LXT_VARIATION_HANDLER VirtualEthernetPairData;
LXT_VARIATION_HANDLER VirtualEthernetPairNamespaceData;

//
// Global constants
//

static const LXT_VARIATION g_LxtVariations[] = {
    {"Permissions Check", SanityPermissionsCheck},
    {"No Device Error Check", EmptyDeviceErrorFromNetlink},
    {"Virtual Bridge IOCTL", VirtualBridgeFromIoctl1},
    {"Virtual Bridge Netlink", VirtualBridgeFromNetlink1},
    {"Virtual Bridge auto name generation", VirtualBridgeAutoName},
    {"Virtual Ethernet Pair", VirtualEthernetPairFromNetlink1},
    {"Virtual Ethernet Pair (part 2)", VirtualEthernetPairFromNetlink2},
    {"Virtual Bridge basic namespace check", VirtualBridgeNamespace1},
    {"Virtual Ethernet Pair basic namespace check", VirtualEthernetPairNamespace1},
    {"Virtual Ethernet Pair namespace check (part 2)", VirtualEthernetPairNamespace2},
    {"Virtual Ethernet Pair namespace check (part 3)", VirtualEthernetPairNamespace3},
    {"Virtual Ethernet Pair namespace link verification", VirtualEthernetPairNamespace4},
    {"Virtual Ethernet Pair namespace socket check", VirtualEthernetPairNamespace5},
    {"Virtual Ethernet Pair simple configuration", VirtualEthernetPairConfigure},

    //
    // Requires firewall manipulation to allow packet traversal
    //

    //{"Virtual Ethernet Pair data check", VirtualEthernetPairData},
    //{"Virtual Ethernet Pair namespace data check", VirtualEthernetPairNamespaceData},

    //
    // Moving physical adapters between namespaces is not currently supported
    //

    //{"Physical device basic namespace check", PhysicalDeviceNamespace1},
};

int VnetTestEntry(int Argc, char* Argv[])

/*++

Routine Description:

    This routine main entry point for the virtual network device test.

Arguments:

    Argc - Supplies the number of command line arguments.

    Argv - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    LXT_ARGS Args;
    int OriginalNetworkNamespaceFd;
    int Result;

    OriginalNetworkNamespaceFd = -1;
    LxtCheckResult(LxtInitialize(Argc, Argv, &Args, LXT_NAME));

    //
    // Since new devices are going to be created/destroyed, move to a new
    // network namespace to try to prevent polluting the root namespace in case
    // of errors.
    //

    LxtCheckErrno(OriginalNetworkNamespaceFd = open("/proc/self/ns/net", 0));
    LxtCheckErrno(unshare(CLONE_NEWNET));

    //
    // Run tests.
    //

    LxtCheckResult(LxtRunVariations(&Args, g_LxtVariations, LXT_COUNT_OF(g_LxtVariations)));

ErrorExit:

    //
    // Try to restore to original network namespace.
    //

    if (OriginalNetworkNamespaceFd > 0)
    {
        setns(OriginalNetworkNamespaceFd, CLONE_NEWNET);
        close(OriginalNetworkNamespaceFd);
    }

    LxtUninitialize();
    return !LXT_SUCCESS(Result);
}

int BindSocketForNetlink(int Socket)

/*++

Routine Description:

    This routine is a helper function to bind a socket for NETLINK use.

Arguments:

    Socket - Supplies the socket to bind.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct sockaddr_nl BindAddress;
    int Result;

    memset(&BindAddress, 0, sizeof(BindAddress));
    BindAddress.nl_family = AF_NETLINK;
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&BindAddress, sizeof(BindAddress)));

    Result = 0;

ErrorExit:
    return Result;
}

int CreateVirtualBridgeViaIoctl(const char* Name)

/*++

Routine Description:

    This routine creates a virtual bridge device via the SIOCBRADDBR ioctl.

Arguments:

    Name - Supplies the name to assign the new bridge.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    int Sock;

    Sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (Sock < 0)
    {
        Result = -1;
        goto ErrorExit;
    }

    Result = ioctl(Sock, SIOCBRADDBR, Name);

ErrorExit:
    if (Sock > 0)
    {
        close(Sock);
    }

    return Result;
}

void CreateVirtualDeviceInfo(const char* Type, const char* Name, const char* DeviceData, int DeviceDataSize, char* Buffer, int* BufferSize)

/*++

Routine Description:

    This routine generates the ifinfomsg and attributes representing a virtual
    device.

Arguments:

    Type - Supplies the optional name of the virtual device type.

    Name - Supplies the name to assign the new device.

    DeviceData - Supplies an optional buffer of device-type specific data.

    DeviceDataSize - Supplies the size in bytes of DeviceData.

    Buffer - Supplies the buffer to store the message. If this is null
        BufferSize will return the number of bytes required.

    BufferSize - Supplies a pointer to the size of the buffer.

Return Value:

    None.

--*/

{

    struct rtattr* Attribute;
    int BufferOffset;
    struct rtattr* LinkAttribute;
    int LinkAttributeMessageOffset;
    int LocalBufferSize;
    int MessageSize;
    int StringLength;

    BufferOffset = 0;
    if (Buffer == NULL)
    {
        LocalBufferSize = 0;
    }
    else
    {
        LocalBufferSize = *BufferSize;
    }

    MessageSize = RTA_ALIGN(sizeof(struct ifinfomsg));
    if (LocalBufferSize >= MessageSize)
    {
        memset(&Buffer[BufferOffset], 0, (MessageSize - BufferOffset));
        BufferOffset = MessageSize;
    }

    StringLength = strlen(Name);
    MessageSize += RTA_SPACE(StringLength + 1);
    if (LocalBufferSize >= MessageSize)
    {
        Attribute = (struct rtattr*)&Buffer[BufferOffset];
        Attribute->rta_len = RTA_LENGTH(StringLength + 1);
        Attribute->rta_type = IFLA_IFNAME;
        memcpy(RTA_DATA(Attribute), Name, StringLength + 1);
        BufferOffset = MessageSize;
    }

    if ((Type != NULL) || ((DeviceData != NULL) && (DeviceDataSize > 0)))
    {
        MessageSize += RTA_SPACE(0);
        if (LocalBufferSize >= MessageSize)
        {
            LinkAttribute = (struct rtattr*)&Buffer[BufferOffset];
            LinkAttribute->rta_type = IFLA_LINKINFO;

            //
            // The length of the link attribute contains other nested
            // attributes, so remember it here and set it later.
            //

            LinkAttributeMessageOffset = BufferOffset;
            BufferOffset = MessageSize;
        }
        else
        {
            LinkAttribute = NULL;
        }

        StringLength = strlen(Type);
        MessageSize += RTA_SPACE(StringLength);
        if (LocalBufferSize >= MessageSize)
        {
            Attribute = (struct rtattr*)&Buffer[BufferOffset];
            Attribute->rta_len = RTA_LENGTH(StringLength);
            Attribute->rta_type = IFLA_INFO_KIND;
            memcpy(RTA_DATA(Attribute), Type, StringLength);
            BufferOffset = MessageSize;
        }

        if ((DeviceData != NULL) && (DeviceDataSize > 0))
        {
            MessageSize += RTA_SPACE(DeviceDataSize);
            if (LocalBufferSize >= MessageSize)
            {
                Attribute = (struct rtattr*)&Buffer[BufferOffset];
                Attribute->rta_len = RTA_LENGTH(DeviceDataSize);
                Attribute->rta_type = IFLA_INFO_DATA;
                memcpy(RTA_DATA(Attribute), DeviceData, DeviceDataSize);
                BufferOffset = MessageSize;
            }
        }

        if (LinkAttribute != NULL)
        {
            LinkAttribute->rta_len = MessageSize - LinkAttributeMessageOffset;
        }
    }

    *BufferSize = MessageSize;
    return;
}

int CreateVirtualBridgeViaNetlink(const char* Name)

/*++

Routine Description:

    This routine creates a new virtual bridge using the netlink RTM_NEWLINK
    message. On success the caller should free the Response buffer.

Arguments:

    Name - Supplies the name to give the new virtual bridge.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    char* DeviceData;
    int DeviceDataSize;
    struct nlmsgerr* ErrorMessage;
    struct nlmsghdr* Response;
    int Result;

    Response = NULL;

    //
    // Determine the size of the device data part of the message.
    //

    CreateVirtualDeviceInfo("bridge", Name, NULL, 0, NULL, &DeviceDataSize);

    //
    // Allocate a buffer to hold the device data.
    //

    DeviceData = LxtAlloc(DeviceDataSize);
    if (DeviceData == NULL)
    {
        Result = -1;
        goto ErrorExit;
    }

    //
    // Fill in the device data buffer.
    //

    CreateVirtualDeviceInfo("bridge", Name, NULL, 0, DeviceData, &DeviceDataSize);

    //
    // Attempt to create the device.
    //

    LxtCheckResult(CreateVirtualDeviceViaNetlink((NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK | NLM_F_REQUEST), DeviceData, DeviceDataSize, &Response));

    //
    // Verify success.
    //

    ErrorMessage = NLMSG_DATA(Response);
    LxtCheckEqual(ErrorMessage->error, 0, "%d");
    Result = 0;

ErrorExit:
    if (Response != NULL)
    {
        LxtFree(Response);
    }

    if (DeviceData != NULL)
    {
        LxtFree(DeviceData);
    }

    return Result;
}

int CreateVirtualDeviceViaNetlink(int Flags, const char* DeviceData, int DeviceDataSize, struct nlmsghdr** Response)

/*++

Routine Description:

    This routine creates a new virtual device using the netlink RTM_NEWLINK
    message. On success the caller should free the Response buffer.

Arguments:

    Flags - Supplies the message flags.

    DeviceData - Supplies the optional device data.

    DeviceDataSize - Supplies the size in bytes of the device data.

    Response - Supplies a pointer to be filled in with newly allocated memory
        holding the response. It is the caller's responsibility to free this
        memory.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct nlmsgerr* ErrorMessage;
    struct nlmsghdr* NextResponseMessage;
    struct nlmsghdr* RequestMessage;
    int RequestMessageSize;
    struct nlmsghdr* ResponseMessage;
    int ResponseMessageSize;
    int ReceiveResult;
    int Result;
    int Socket;

    //
    // Allocate space for the request and the response.
    //

    RequestMessageSize = NLMSG_SPACE(DeviceDataSize);
    RequestMessage = LxtAlloc(RequestMessageSize);
    if (RequestMessage == NULL)
    {
        Result = ENOMEM;
        goto ErrorExit;
    }

    ResponseMessageSize = RequestMessageSize + sizeof(*ErrorMessage);
    ResponseMessage = LxtAlloc(ResponseMessageSize);
    if (RequestMessage == NULL)
    {
        Result = ENOMEM;
        goto ErrorExit;
    }

    //
    // Create and bind socket.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    LxtCheckErrno(BindSocketForNetlink(Socket));

    //
    // Create a RTM_NEWLINK request.
    //

    memset(RequestMessage, 0, sizeof(*RequestMessage));
    RequestMessage->nlmsg_len = RequestMessageSize;
    RequestMessage->nlmsg_type = RTM_NEWLINK;
    RequestMessage->nlmsg_seq = LXT_REQUEST_SEQUENCE;
    RequestMessage->nlmsg_flags = Flags;
    memcpy(NLMSG_DATA(RequestMessage), DeviceData, DeviceDataSize);
    LxtCheckErrno(sendto(Socket, RequestMessage, RequestMessageSize, 0, NULL, 0));

    //
    // Get the response.
    //

    memset(ResponseMessage, 0, ResponseMessageSize);
    LxtCheckErrno(ReceiveResult = recvfrom(Socket, ResponseMessage, ResponseMessageSize, 0, NULL, 0));

    LxtCheckTrue(NLMSG_OK(ResponseMessage, ReceiveResult));
    LxtCheckEqual(ResponseMessage->nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckGreater((ResponseMessage->nlmsg_len + 1), NLMSG_PAYLOAD(ResponseMessage, 0), "%d");

    ErrorMessage = NLMSG_DATA(ResponseMessage);
    if (ErrorMessage->error < 0)
    {

        //
        // On error, the entire message should be returned.
        //

        LxtCheckEqual(ResponseMessage->nlmsg_len, ResponseMessageSize, "%d");
    }
    else
    {

        //
        // On success, just the header of the original message is expected to
        // be returned, which is already included in the error message size.
        //

        LxtCheckEqual(ResponseMessage->nlmsg_len, NLMSG_SPACE(sizeof(*ErrorMessage)), "%d");
    }

    LxtCheckEqual(ResponseMessage->nlmsg_flags, 0, "%d");
    LxtCheckEqual(ResponseMessage->nlmsg_seq, LXT_REQUEST_SEQUENCE, "%d");
    LxtCheckEqual(ResponseMessage->nlmsg_pid, getpid(), "%d");
    NextResponseMessage = NLMSG_NEXT(ResponseMessage, ReceiveResult);
    LxtCheckTrue(NLMSG_OK(NextResponseMessage, ReceiveResult) == FALSE);
    Result = 0;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    if (RequestMessage != NULL)
    {
        LxtFree(RequestMessage);
    }

    if (Result != 0)
    {
        if (ResponseMessage != NULL)
        {
            LxtFree(ResponseMessage);
        }

        errno = Result;
        return -1;
    }
    else
    {
        *Response = ResponseMessage;
        return 0;
    }
}

int DeleteVirtualBridgeViaIoctl(const char* Name)

/*++

Routine Description:

    This routine deletes a virtual bridge device via the SIOCBRDELBR ioctl.

Arguments:

    Name - Supplies the name to assign the new bridge.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;
    int Sock;

    Sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (Sock < 0)
    {
        Result = -1;
        goto ErrorExit;
    }

    Result = ioctl(Sock, SIOCBRDELBR, Name);

ErrorExit:
    if (Sock > 0)
    {
        close(Sock);
    }

    return Result;
}

int DeleteVirtualDeviceViaNetlink(const char* Name)

/*++

Routine Description:

    This routine removes a virtual device using the netlink RTM_DELLINK
    message.

Arguments:

    Name - Supplies the name of the device to delete.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    int InterfaceIndex;
    struct nlmsghdr* NextResponseMessage;
    int ReceiveResult;
    struct
    {
        struct nlmsghdr Header;
        struct ifinfomsg Message __attribute__((aligned(NLMSG_ALIGNTO)));
    } RequestMessage;
    struct
    {
        struct nlmsghdr Header;
        struct nlmsgerr Error __attribute__((aligned(NLMSG_ALIGNTO)));
    } ResponseMessage;
    int Result;
    int Socket;

    LxtCheckErrno(InterfaceIndex = GetNetworkInterfaceIndex(Name));

    //
    // Create and bind NETLINK socket.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    LxtCheckErrno(BindSocketForNetlink(Socket));

    //
    // Create a RTM_DELLINK request.
    //

    memset(&RequestMessage, 0, sizeof(RequestMessage));
    RequestMessage.Header.nlmsg_len = sizeof(RequestMessage);
    RequestMessage.Header.nlmsg_type = RTM_DELLINK;
    RequestMessage.Header.nlmsg_seq = LXT_REQUEST_SEQUENCE;
    RequestMessage.Header.nlmsg_flags = NLM_F_ACK | NLM_F_REQUEST;
    RequestMessage.Message.ifi_index = InterfaceIndex;
    LxtCheckErrno(sendto(Socket, &RequestMessage, sizeof(RequestMessage), 0, NULL, 0));

    //
    // Get the response.
    //

    memset(&ResponseMessage, 0, sizeof(ResponseMessage));
    LxtCheckErrno(ReceiveResult = recvfrom(Socket, &ResponseMessage, sizeof(ResponseMessage), 0, NULL, 0));

    LxtCheckTrue(NLMSG_OK(&ResponseMessage.Header, ReceiveResult));
    LxtCheckEqual(ResponseMessage.Header.nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_len, sizeof(ResponseMessage), "%d");

    LxtCheckEqual(ResponseMessage.Header.nlmsg_flags, 0, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_seq, LXT_REQUEST_SEQUENCE, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_pid, getpid(), "%d");
    LxtCheckEqual(ResponseMessage.Error.error, 0, "%d");
    NextResponseMessage = NLMSG_NEXT(&ResponseMessage.Header, ReceiveResult);
    LxtCheckTrue(NLMSG_OK(NextResponseMessage, ReceiveResult) == FALSE);
    Result = 0;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int CreateVirtualEthernetPairViaNetlink(const char* Name, const char* PeerName)

/*++

Routine Description:

    This routine creates a new virtual bridge using the netlink RTM_NEWLINK
    message. On success the caller should free the Response buffer.

Arguments:

    Name - Supplies the name to give the new virtual ethernet.

    PeerName - Supplies the name to give the other end of the new virtual
        ethernet pair.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct rtattr* Attribute;
    char* DeviceData;
    int DeviceDataSize;
    struct nlmsgerr* ErrorMessage;
    char* PeerDeviceData;
    int PeerDeviceDataSize;
    struct nlmsghdr* Response;
    int Result;

    Response = NULL;

    //
    // Determine the size of the peer device data part of the message.
    //

    CreateVirtualDeviceInfo(NULL, PeerName, NULL, 0, NULL, &PeerDeviceDataSize);

    //
    // Allocate a buffer to hold the peer device data.
    //

    PeerDeviceData = LxtAlloc(RTA_SPACE(PeerDeviceDataSize));
    if (PeerDeviceData == NULL)
    {
        Result = -1;
        goto ErrorExit;
    }

    //
    // Fill in the peer device data buffer.
    //

    Attribute = (struct rtattr*)PeerDeviceData;
    Attribute->rta_len = RTA_LENGTH(PeerDeviceDataSize);
    Attribute->rta_type = VETH_INFO_PEER;
    CreateVirtualDeviceInfo(NULL, PeerName, NULL, 0, RTA_DATA(Attribute), &PeerDeviceDataSize);

    //
    // Determine the size of the device data part of the message.
    //

    CreateVirtualDeviceInfo("veth", Name, PeerDeviceData, Attribute->rta_len, NULL, &DeviceDataSize);

    //
    // Allocate a buffer to hold the peer device data.
    //

    DeviceData = LxtAlloc(DeviceDataSize);
    if (DeviceData == NULL)
    {
        Result = -1;
        goto ErrorExit;
    }

    //
    // Fill in the peer device data buffer.
    //

    CreateVirtualDeviceInfo("veth", Name, PeerDeviceData, Attribute->rta_len, DeviceData, &DeviceDataSize);

    //
    // Attempt to create the device.
    //

    LxtCheckResult(CreateVirtualDeviceViaNetlink((NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK | NLM_F_REQUEST), DeviceData, DeviceDataSize, &Response));

    //
    // Verify success.
    //

    ErrorMessage = NLMSG_DATA(Response);
    LxtCheckEqual(ErrorMessage->error, 0, "%d");
    Result = 0;

ErrorExit:
    if (Response != NULL)
    {
        LxtFree(Response);
    }

    if (DeviceData != NULL)
    {
        LxtFree(DeviceData);
    }

    if (PeerDeviceData != NULL)
    {
        LxtFree(PeerDeviceData);
    }

    return Result;
}

int GetNetworkInterfaceIndex(const char* Name)

/*++

Routine Description:

    This routine returns the network interface index of a device.

Arguments:

    Name - Supplies the name of the device

Return Value:

    Returns the index on success, -1 on failure.

--*/

{

    struct ifreq InterfaceRequest;
    int Result;
    int Socket;

    Socket = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (Socket < 0)
    {
        Result = -1;
        goto ErrorExit;
    }

    memset(&InterfaceRequest, 0, sizeof(InterfaceRequest));
    strcpy(InterfaceRequest.ifr_name, Name);
    Result = ioctl(Socket, SIOCGIFINDEX, &InterfaceRequest);
    LxtClose(Socket);
    if (Result < 0)
    {
        goto ErrorExit;
    }

    Result = InterfaceRequest.ifr_ifindex;

ErrorExit:
    return Result;
}

int EmptyDeviceErrorFromNetlink(PLXT_ARGS Args)

/*++

Routine Description:

    This routine sends a create message with no device described. This pattern
    is used by tools like 'ip' to determine if the system supports virtual
    network device creation.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct ifinfomsg DeviceData;
    struct nlmsgerr* ErrorMessage;
    struct nlmsghdr* Response;
    int Result;

    Response = NULL;
    memset(&DeviceData, 0, sizeof(DeviceData));
    LxtCheckResult(CreateVirtualDeviceViaNetlink((NLM_F_REQUEST | NLM_F_ACK), (const char*)&DeviceData, sizeof(DeviceData), &Response));

    ErrorMessage = NLMSG_DATA(Response);
    LxtCheckEqual(ErrorMessage->error, -ENODEV, "%d");
    Result = 0;

ErrorExit:
    if (Response != NULL)
    {
        LxtFree(Response);
    }

    return Result;
}

int PhysicalDeviceNamespace1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine does basic network namespace sanity testing with a physical
    device (assumes existence of eth0):
        1) creates a new namespace
        2) attempt to move eth0 to new namespace
        3) close new namespace
        4) verify device was returned to the root namespace

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int NewNetworkNamespaceFd;
    int OriginalNetworkNamespaceFd;
    int RootNetworkNamespaceFd;
    int Response;
    int Result;

    NewNetworkNamespaceFd = 0;
    OriginalNetworkNamespaceFd = 0;

    //
    // Open file descriptor of default network namespace.
    //

    LxtCheckErrno(OriginalNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Open file descriptor of the root network namespace.
    //

    LxtCheckErrno(RootNetworkNamespaceFd = open("/proc/1/ns/net", 0));

    //
    // Switch to a new network namespace.
    //

    LxtCheckErrno(unshare(CLONE_NEWNET));

    //
    // Open file descriptor of the new network namespace.
    //

    LxtCheckErrno(NewNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Switch to root network namespace.
    //

    LxtCheckErrno(setns(RootNetworkNamespaceFd, CLONE_NEWNET));

    //
    // Try to move eth0 to the new network namespace.
    //

    LxtCheckResult(SetVirtualDeviceAttributeViaNetlink("eth0", IFLA_NET_NS_FD, NewNetworkNamespaceFd, &Response));

    LxtCheckEqual(Response, 0, "%d");

    //
    // Verify that the device is gone.
    //

    LxtCheckErrnoFailure(GetNetworkInterfaceIndex("eth0"), ENODEV);

    //
    // Close the new network namespace. This should restore any physical
    // devices to the root network namespace.
    //

    LxtCheckClose(NewNetworkNamespaceFd);

    //
    // Pause for a bit to allow background processing to occur.
    //

    sleep(1);

    //
    // Verify that the device is back.
    //

    LxtCheckErrno(GetNetworkInterfaceIndex("eth0"));

    //
    // Restore back to default network namespace.
    //

    LxtCheckErrno(setns(OriginalNetworkNamespaceFd, CLONE_NEWNET));

ErrorExit:
    if (NewNetworkNamespaceFd > 0)
    {
        close(NewNetworkNamespaceFd);
    }

    if (RootNetworkNamespaceFd > 0)
    {
        close(RootNetworkNamespaceFd);
    }

    if (OriginalNetworkNamespaceFd > 0)
    {
        close(OriginalNetworkNamespaceFd);
    }

    return Result;
}

int SanityPermissionsCheck(PLXT_ARGS Args)

/*++

Routine Description:

    This routine does a simple operation to check that the current user has
    appropriate permissions. This should be run as the first test to give a
    quick error to the user that typically the entire test needs to be run
    with sudo.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct ifinfomsg DeviceData;
    struct nlmsgerr* ErrorMessage;
    struct nlmsghdr* Response;
    int Result;

    Response = NULL;
    memset(&DeviceData, 0, sizeof(DeviceData));
    LxtCheckResult(CreateVirtualDeviceViaNetlink((NLM_F_REQUEST | NLM_F_ACK), (const char*)&DeviceData, sizeof(DeviceData), &Response));

    ErrorMessage = NLMSG_DATA(Response);
    if (ErrorMessage->error == -1)
    {
        LxtLogError("Make sure test is run with sudo!");
        Result = -1;
        goto ErrorExit;
    }

    Result = 0;

ErrorExit:
    if (Response != NULL)
    {
        LxtFree(Response);
    }

    return Result;
}

int SetIpAddress(const char* InterfaceName, const struct in_addr* AddressIpv4, char PrefixLength)

/*++

Routine Description:

    This routine adds an IP address to a network interface.

Arguments:

    InterfaceName - Supplies the interface name.

    AddressIpv4 - Supplies the IP address.

    PrefixLength - Supplies the prefix length of the address.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct nlmsgerr* ErrorMessage;
    int InterfaceIndex;
    int ReceiveResult;
    struct
    {
        struct nlmsghdr Header;
        struct ifaddrmsg Message __attribute__((aligned(NLMSG_ALIGNTO)));
        struct
        {
            struct rtattr RtHeader __attribute__((aligned(RTA_ALIGNTO)));
            struct in_addr AddressIpv4 __attribute__((aligned(RTA_ALIGNTO)));
        } AddressAttribute;
        struct
        {
            struct rtattr RtHeader __attribute__((aligned(RTA_ALIGNTO)));
            struct in_addr AddressIpv4 __attribute__((aligned(RTA_ALIGNTO)));
        } LocalAddressAttribute;
    } RequestMessage;
    struct
    {
        struct nlmsghdr Header;
        struct nlmsgerr Error __attribute__((aligned(NLMSG_ALIGNTO)));
    } ResponseMessage;
    int Result;
    int Socket;

    LxtCheckErrno(InterfaceIndex = GetNetworkInterfaceIndex(InterfaceName));

    //
    // Create and bind NETLINK socket.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    LxtCheckErrno(BindSocketForNetlink(Socket));

    //
    // Create a RTM_NEWLINK request.
    //

    memset(&RequestMessage, 0, sizeof(RequestMessage));
    RequestMessage.Header.nlmsg_len = sizeof(RequestMessage);
    RequestMessage.Header.nlmsg_type = RTM_NEWADDR;
    RequestMessage.Header.nlmsg_seq = LXT_REQUEST_SEQUENCE;
    RequestMessage.Header.nlmsg_flags = NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK | NLM_F_REQUEST;

    RequestMessage.Message.ifa_family = AF_INET;
    RequestMessage.Message.ifa_prefixlen = PrefixLength;
    RequestMessage.Message.ifa_index = InterfaceIndex;
    RequestMessage.AddressAttribute.RtHeader.rta_len = RTA_LENGTH(sizeof(*AddressIpv4));

    RequestMessage.AddressAttribute.RtHeader.rta_type = IFA_ADDRESS;
    memcpy(&RequestMessage.AddressAttribute.AddressIpv4, AddressIpv4, sizeof(*AddressIpv4));

    RequestMessage.LocalAddressAttribute.RtHeader.rta_len = RTA_LENGTH(sizeof(*AddressIpv4));

    RequestMessage.LocalAddressAttribute.RtHeader.rta_type = IFA_LOCAL;
    memcpy(&RequestMessage.LocalAddressAttribute.AddressIpv4, AddressIpv4, sizeof(*AddressIpv4));

    LxtCheckErrno(sendto(Socket, &RequestMessage, sizeof(RequestMessage), 0, NULL, 0));

    //
    // Get the response.
    //

    memset(&ResponseMessage, 0, sizeof(ResponseMessage));
    LxtCheckErrno(ReceiveResult = recvfrom(Socket, &ResponseMessage, sizeof(ResponseMessage), 0, NULL, 0));

    LxtCheckEqual(ReceiveResult, sizeof(ResponseMessage), "%d");
    if (ResponseMessage.Header.nlmsg_len != ReceiveResult)
    {
        LxtCheckGreater(ResponseMessage.Header.nlmsg_len, ReceiveResult, "%d");
    }

    LxtCheckEqual(ResponseMessage.Header.nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_flags, 0, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_seq, LXT_REQUEST_SEQUENCE, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_pid, getpid(), "%d");
    LxtCheckEqual(ResponseMessage.Error.error, 0, "%d");
    Result = 0;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SetRoute(const char* InterfaceName, const struct in_addr* AddressIpv4, int PrefixLength)

/*++

Routine Description:

    This routine adds a route to a network interface.

Arguments:

    InterfaceName - Supplies the interface name.

    AddressIpv4 - Supplies the IP address.

    PrefixLength - Supplies the prefix length of AddressIpv4.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct nlmsgerr* ErrorMessage;
    int InterfaceIndex;
    int ReceiveResult;
    struct
    {
        struct nlmsghdr Header;
        struct rtmsg Message __attribute__((aligned(NLMSG_ALIGNTO)));
        struct
        {
            struct rtattr RtHeader __attribute__((aligned(RTA_ALIGNTO)));
            struct in_addr AddressIpv4 __attribute__((aligned(RTA_ALIGNTO)));
        } DestAttribute;
        struct
        {
            struct rtattr RtHeader __attribute__((aligned(RTA_ALIGNTO)));
            int InterfaceIndex __attribute__((aligned(RTA_ALIGNTO)));
        } IndexAttribute;
    } RequestMessage;
    struct
    {
        struct nlmsghdr Header;
        struct nlmsgerr Error __attribute__((aligned(NLMSG_ALIGNTO)));
    } ResponseMessage;
    int Result;
    int Socket;

    LxtCheckErrno(InterfaceIndex = GetNetworkInterfaceIndex(InterfaceName));

    //
    // Create and bind NETLINK socket.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    LxtCheckErrno(BindSocketForNetlink(Socket));

    //
    // Create a RTM_NEWROUTE request.
    //

    memset(&RequestMessage, 0, sizeof(RequestMessage));
    RequestMessage.Header.nlmsg_len = sizeof(RequestMessage);
    RequestMessage.Header.nlmsg_type = RTM_NEWROUTE;
    RequestMessage.Header.nlmsg_seq = LXT_REQUEST_SEQUENCE;
    RequestMessage.Header.nlmsg_flags = NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK | NLM_F_REQUEST;

    RequestMessage.Message.rtm_family = AF_INET;
    RequestMessage.Message.rtm_dst_len = PrefixLength;
    RequestMessage.Message.rtm_table = RT_TABLE_MAIN;
    RequestMessage.Message.rtm_protocol = RTPROT_BOOT;
    RequestMessage.Message.rtm_scope = RT_SCOPE_LINK;
    RequestMessage.Message.rtm_type = RTA_DST;

    RequestMessage.DestAttribute.RtHeader.rta_len = RTA_LENGTH(sizeof(*AddressIpv4));

    RequestMessage.DestAttribute.RtHeader.rta_type = RTA_DST;
    memcpy(&RequestMessage.DestAttribute.AddressIpv4, AddressIpv4, sizeof(*AddressIpv4));

    RequestMessage.IndexAttribute.RtHeader.rta_len = RTA_LENGTH(sizeof(int));

    RequestMessage.IndexAttribute.RtHeader.rta_type = RTA_OIF;
    RequestMessage.IndexAttribute.InterfaceIndex = InterfaceIndex;

    LxtCheckErrno(sendto(Socket, &RequestMessage, sizeof(RequestMessage), 0, NULL, 0));

    //
    // Get the response.
    //

    memset(&ResponseMessage, 0, sizeof(ResponseMessage));
    LxtCheckErrno(ReceiveResult = recvfrom(Socket, &ResponseMessage, sizeof(ResponseMessage), 0, NULL, 0));

    LxtCheckEqual(ReceiveResult, sizeof(ResponseMessage), "%d");
    if (ResponseMessage.Header.nlmsg_len != ReceiveResult)
    {
        LxtCheckGreater(ResponseMessage.Header.nlmsg_len, ReceiveResult, "%d");
    }

    LxtCheckEqual(ResponseMessage.Header.nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_flags, 0, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_seq, LXT_REQUEST_SEQUENCE, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_pid, getpid(), "%d");
    LxtCheckEqual(ResponseMessage.Error.error, 0, "%d");
    Result = 0;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SetVirtualDeviceAttributeViaNetlink(const char* Name, int AttributeType, int AttributeValue, int* Response)

/*++

Routine Description:

    This routine attempts to move a virtual device to the network namespace
    referenced by the NamespaceFd.

Arguments:

    Name - Supplies the name of the virtual device.

    AttributeType - Supplies the attribute type.

    AttributeValue - Supplies the attribute value.

    NetworkNamespaceFd - Supplies the file descriptor of a network namespace.

    Response - Supplies a pointer to receive the response errno.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct nlmsgerr* ErrorMessage;
    int InterfaceIndex;
    int ReceiveResult;
    struct
    {
        struct nlmsghdr Header;
        struct ifinfomsg Message __attribute__((aligned(NLMSG_ALIGNTO)));
        struct
        {
            struct rtattr RtHeader __attribute__((aligned(RTA_ALIGNTO)));
            int RtValue __attribute__((aligned(RTA_ALIGNTO)));
        } Attribute;
    } RequestMessage;
    struct
    {
        struct nlmsghdr Header;
        struct nlmsgerr Error __attribute__((aligned(NLMSG_ALIGNTO)));
    } ResponseMessage;
    int Result;
    int Socket;

    LxtCheckErrno(InterfaceIndex = GetNetworkInterfaceIndex(Name));

    //
    // Create and bind NETLINK socket.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    LxtCheckErrno(BindSocketForNetlink(Socket));

    //
    // Create a RTM_NEWLINK update request.
    //

    memset(&RequestMessage, 0, sizeof(RequestMessage));
    RequestMessage.Header.nlmsg_len = sizeof(RequestMessage);
    RequestMessage.Header.nlmsg_type = RTM_NEWLINK;
    RequestMessage.Header.nlmsg_seq = LXT_REQUEST_SEQUENCE;
    RequestMessage.Header.nlmsg_flags = NLM_F_ACK | NLM_F_REQUEST;
    RequestMessage.Message.ifi_index = InterfaceIndex;
    RequestMessage.Attribute.RtHeader.rta_len = RTA_LENGTH(sizeof(int));
    RequestMessage.Attribute.RtHeader.rta_type = AttributeType;
    RequestMessage.Attribute.RtValue = AttributeValue;
    LxtCheckErrno(sendto(Socket, &RequestMessage, sizeof(RequestMessage), 0, NULL, 0));

    //
    // Get the response.
    //

    memset(&ResponseMessage, 0, sizeof(ResponseMessage));
    LxtCheckErrno(ReceiveResult = recvfrom(Socket, &ResponseMessage, sizeof(ResponseMessage), 0, NULL, 0));

    LxtCheckEqual(ReceiveResult, sizeof(ResponseMessage), "%d");
    if (ResponseMessage.Header.nlmsg_len != ReceiveResult)
    {
        LxtCheckGreater(ResponseMessage.Header.nlmsg_len, ReceiveResult, "%d");
    }

    LxtCheckEqual(ResponseMessage.Header.nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_flags, 0, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_seq, LXT_REQUEST_SEQUENCE, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_pid, getpid(), "%d");
    *Response = ResponseMessage.Error.error;
    Result = 0;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int SetVirtualDeviceFlagViaNetlink(const char* Name, int Flag, bool IsFlagEnabled, int* Response)

/*++

Routine Description:

    This routine toggles network interface flags (e.g. up/down).

Arguments:

    Name - Supplies the name of the virtual device.

    Flag - Supplies the flag to toggle.

    IsFlagEnabled - Supplies the state to which the flag should be set.

    Response - Supplies a pointer to receive the response errno.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    struct nlmsgerr* ErrorMessage;
    int InterfaceIndex;
    int ReceiveResult;
    struct
    {
        struct nlmsghdr Header;
        struct ifinfomsg Message __attribute__((aligned(NLMSG_ALIGNTO)));
    } RequestMessage;
    struct
    {
        struct nlmsghdr Header;
        struct nlmsgerr Error __attribute__((aligned(NLMSG_ALIGNTO)));
    } ResponseMessage;
    int Result;
    int Socket;

    LxtCheckErrno(InterfaceIndex = GetNetworkInterfaceIndex(Name));

    //
    // Create and bind NETLINK socket.
    //

    LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
    LxtCheckErrno(BindSocketForNetlink(Socket));

    //
    // Create a RTM_NEWLINK update request.
    //

    memset(&RequestMessage, 0, sizeof(RequestMessage));
    RequestMessage.Header.nlmsg_len = sizeof(RequestMessage);
    RequestMessage.Header.nlmsg_type = RTM_NEWLINK;
    RequestMessage.Header.nlmsg_seq = LXT_REQUEST_SEQUENCE;
    RequestMessage.Header.nlmsg_flags = NLM_F_ACK | NLM_F_REQUEST;
    RequestMessage.Message.ifi_index = InterfaceIndex;
    RequestMessage.Message.ifi_change = Flag;
    if (IsFlagEnabled)
    {
        RequestMessage.Message.ifi_flags = Flag;
    }

    LxtCheckErrno(sendto(Socket, &RequestMessage, sizeof(RequestMessage), 0, NULL, 0));

    //
    // Get the response.
    //

    memset(&ResponseMessage, 0, sizeof(ResponseMessage));
    LxtCheckErrno(ReceiveResult = recvfrom(Socket, &ResponseMessage, sizeof(ResponseMessage), 0, NULL, 0));

    LxtCheckEqual(ReceiveResult, sizeof(ResponseMessage), "%d");
    if (ResponseMessage.Header.nlmsg_len != ReceiveResult)
    {
        LxtCheckGreater(ResponseMessage.Header.nlmsg_len, ReceiveResult, "%d");
    }

    LxtCheckEqual(ResponseMessage.Header.nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_flags, 0, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_seq, LXT_REQUEST_SEQUENCE, "%d");
    LxtCheckEqual(ResponseMessage.Header.nlmsg_pid, getpid(), "%d");
    *Response = ResponseMessage.Error.error;
    Result = 0;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    return Result;
}

int VerifyRouteLinkDoesNotExist(
    const char* InterfaceName,
    /* optional */ int* SocketToUse)

/*++

Routine Description:

    This routine attempts to retrieve the link information for a network
    interface, expecting that interface not to exist.

Arguments:

    InterfaceName - Supplies the name of the network interface.

    SocketToUse - Supplies an optional socket bound to NETLINK to use for the
        query.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    bool CloseSocket;
    struct nlmsgerr* ErrorMessage;
    struct nlmsghdr* Header;
    int InterfaceIndex;
    size_t InterfaceNameLength;
    char ReceiveBuffer[5000];
    int ReceiveResult;
    int Result;
    int Socket;

    struct _RequestMessage
    {
        struct nlmsghdr Header;
        struct ifinfomsg Message __attribute__((aligned(NLMSG_ALIGNTO)));
        struct
        {
            struct rtattr RtHeader __attribute__((aligned(RTA_ALIGNTO)));
            char InterfaceName[32] __attribute__((aligned(RTA_ALIGNTO)));
        } Attribute;
    } RequestMessage;

    if (SocketToUse != NULL)
    {
        CloseSocket = false;
        Socket = *SocketToUse;
    }
    else
    {

        //
        // Create and bind socket. Create a RTM_GETLINK request.
        //

        LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
        CloseSocket = true;
        LxtCheckErrno(BindSocketForNetlink(Socket));
    }

    memset(&RequestMessage, 0, sizeof(RequestMessage));
    RequestMessage.Header.nlmsg_type = RTM_GETLINK;
    RequestMessage.Header.nlmsg_seq = LXT_REQUEST_SEQUENCE;
    ;
    RequestMessage.Message.ifi_family = AF_NETLINK;
    RequestMessage.Header.nlmsg_flags = NLM_F_REQUEST;
    InterfaceNameLength = strlen(InterfaceName) + 1;
    LxtCheckGreaterOrEqual(sizeof(RequestMessage.Attribute.InterfaceName), InterfaceNameLength, "%lu");

    RequestMessage.Attribute.RtHeader.rta_len = RTA_LENGTH(InterfaceNameLength);
    RequestMessage.Attribute.RtHeader.rta_type = IFLA_IFNAME;
    memcpy(RequestMessage.Attribute.InterfaceName, InterfaceName, InterfaceNameLength);

    RequestMessage.Header.nlmsg_len = offsetof(struct _RequestMessage, Attribute.InterfaceName) + InterfaceNameLength;

    LxtCheckErrno(sendto(Socket, &RequestMessage, RequestMessage.Header.nlmsg_len, 0, NULL, 0));

    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));

    Header = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckTrue(NLMSG_OK(Header, ReceiveResult));
    LxtCheckEqual(Header->nlmsg_type, NLMSG_ERROR, "%d");
    LxtCheckGreater((Header->nlmsg_len + 1), NLMSG_PAYLOAD(Header, 0), "%d");

    ErrorMessage = NLMSG_DATA(Header);
    LxtCheckEqual(ErrorMessage->error, -ENODEV, "%d");

ErrorExit:
    if (CloseSocket)
    {
        close(Socket);
    }

    return Result;
}

int VerifyRouteLinkExists(
    const char* InterfaceName,
    /* optional */ int* SocketToUse)

/*++

Routine Description:

    This routine retrieves the link information for a network interface.

Arguments:

    InterfaceName - Supplies the name of the network interface.

    SocketToUse - Supplies an optional socket bound to NETLINK to use for the
        query.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    socklen_t AddressLength;
    bool CloseSocket;
    struct nlmsghdr* Header;
    struct ifinfomsg* InterfaceInfo;
    int InterfaceIndex;
    size_t InterfaceNameLength;
    char ReceiveBuffer[5000];
    int ReceiveResult;
    int Result;
    int Socket;

    struct _RequestMessage
    {
        struct nlmsghdr Header;
        struct ifinfomsg Message __attribute__((aligned(NLMSG_ALIGNTO)));
        struct
        {
            struct rtattr RtHeader __attribute__((aligned(RTA_ALIGNTO)));
            char InterfaceName[32] __attribute__((aligned(RTA_ALIGNTO)));
        } Attribute;
    } RequestMessage;

    if (SocketToUse != NULL)
    {
        CloseSocket = false;
        Socket = *SocketToUse;
    }
    else
    {

        //
        // Create and bind socket. Create a RTM_GETLINK request.
        //

        LxtCheckErrno(Socket = socket(AF_NETLINK, SOCK_RAW, 0));
        CloseSocket = true;
        LxtCheckErrno(BindSocketForNetlink(Socket));
    }

    memset(&RequestMessage, 0, sizeof(RequestMessage));
    RequestMessage.Header.nlmsg_type = RTM_GETLINK;
    RequestMessage.Header.nlmsg_seq = LXT_REQUEST_SEQUENCE;
    ;
    RequestMessage.Message.ifi_family = AF_NETLINK;
    RequestMessage.Header.nlmsg_flags = NLM_F_REQUEST;
    InterfaceNameLength = strlen(InterfaceName) + 1;
    LxtCheckGreaterOrEqual(sizeof(RequestMessage.Attribute.InterfaceName), InterfaceNameLength, "%lu");

    RequestMessage.Attribute.RtHeader.rta_len = RTA_LENGTH(InterfaceNameLength);
    RequestMessage.Attribute.RtHeader.rta_type = IFLA_IFNAME;
    memcpy(RequestMessage.Attribute.InterfaceName, InterfaceName, InterfaceNameLength);

    RequestMessage.Header.nlmsg_len = offsetof(struct _RequestMessage, Attribute.InterfaceName) + InterfaceNameLength;

    LxtCheckErrno(sendto(Socket, &RequestMessage, RequestMessage.Header.nlmsg_len, 0, NULL, 0));

    LxtCheckErrno(ReceiveResult = recv(Socket, ReceiveBuffer, sizeof(ReceiveBuffer), 0));

    Header = (struct nlmsghdr*)ReceiveBuffer;
    LxtCheckTrue(NLMSG_OK(Header, ReceiveResult));
    LxtCheckEqual(Header->nlmsg_type, RTM_NEWLINK, "%d");
    LxtCheckGreaterOrEqual((Header->nlmsg_len + 1), NLMSG_PAYLOAD(Header, sizeof(*InterfaceInfo)), "%d");

    InterfaceInfo = NLMSG_DATA(Header);
    if (SocketToUse == NULL)
    {
        LxtCheckErrno(InterfaceIndex = GetNetworkInterfaceIndex(InterfaceName));
        LxtCheckEqual(InterfaceIndex, InterfaceInfo->ifi_index, "%d");
    }

ErrorExit:
    if (CloseSocket)
    {
        close(Socket);
    }

    return Result;
}

int VirtualBridgeAutoName(PLXT_ARGS Args)

/*++

Routine Description:

    This routine creates and deletes new bridges, allowing the system to
    assign them default "bridgexx" names.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    bool DeleteDeviceOne;
    bool DeleteDeviceTwo;
    int Result;

    DeleteDeviceOne = false;
    DeleteDeviceTwo = false;

    LxtCheckResult(CreateVirtualBridgeViaNetlink(""));
    DeleteDeviceOne = true;
    LxtCheckErrno(GetNetworkInterfaceIndex("bridge0"));
    LxtCheckResult(CreateVirtualBridgeViaNetlink(""));
    DeleteDeviceTwo = true;
    LxtCheckErrno(GetNetworkInterfaceIndex("bridge1"));

ErrorExit:
    if (DeleteDeviceTwo)
    {
        (void)DeleteVirtualDeviceViaNetlink("bridge1");
    }

    if (DeleteDeviceOne)
    {
        (void)DeleteVirtualDeviceViaNetlink("bridge0");
    }

    return Result;
}

int VirtualBridgeFromIoctl1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine creates and deletes a new bridge using UNIX socket ioctls.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    int Result;

    LxtCheckResult(CreateVirtualBridgeViaIoctl("testbridge"));
    LxtCheckErrno(GetNetworkInterfaceIndex("testbridge"));
    LxtCheckResult(DeleteVirtualBridgeViaIoctl("testbridge"));
    LxtCheckErrnoFailure(GetNetworkInterfaceIndex("testbridge"), ENODEV);

ErrorExit:
    return Result;
}

int VirtualBridgeFromNetlink1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine creates and deletes a new bridge using netlink messages.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    bool DeleteDevice;
    int Result;

    DeleteDevice = false;

    LxtCheckResult(CreateVirtualBridgeViaNetlink("testbridge"));
    DeleteDevice = true;
    LxtCheckErrno(GetNetworkInterfaceIndex("testbridge"));
    LxtCheckResult(DeleteVirtualDeviceViaNetlink("testbridge"));
    DeleteDevice = false;
    LxtCheckErrnoFailure(GetNetworkInterfaceIndex("testbridge"), ENODEV);

ErrorExit:
    if (DeleteDevice)
    {
        (void)DeleteVirtualDeviceViaNetlink("testbridge");
    }

    return Result;
}

int VirtualBridgeNamespace1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine does basic network namespace sanity testing with a bridge:
        1) creates a new virtual bridge device
        1) creates a new namespace
        3) try to move the bridge into the other namespace - should fail.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    bool DeleteDevice;
    int NewNetworkNamespaceFd;
    int OriginalNetworkNamespaceFd;
    int Response;
    int Result;

    DeleteDevice = false;
    NewNetworkNamespaceFd = 0;
    OriginalNetworkNamespaceFd = 0;

    //
    // Open file descriptor of default network namespace.
    //

    LxtCheckErrno(OriginalNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Create a new virtual bridge.
    //

    LxtCheckResult(CreateVirtualBridgeViaNetlink("testbridge"));
    DeleteDevice = true;

    //
    // Switch to a new network namespace.
    //

    LxtCheckErrno(unshare(CLONE_NEWNET));

    //
    // Open file descriptor of the new network namespace.
    //

    LxtCheckErrno(NewNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Switch back to original network namespace.
    //

    LxtCheckErrno(setns(OriginalNetworkNamespaceFd, CLONE_NEWNET));

    //
    // Try to move the bridge into the new namespace. This should fail as
    // bridges are not allowed to move between namespaces.
    //

    LxtCheckResult(SetVirtualDeviceAttributeViaNetlink("testbridge", IFLA_NET_NS_FD, NewNetworkNamespaceFd, &Response));

    LxtCheckEqual(Response, -EINVAL, "%d");

    //
    // Delete the device.
    //

    LxtCheckResult(DeleteVirtualDeviceViaNetlink("testbridge"));
    DeleteDevice = false;

ErrorExit:
    if (NewNetworkNamespaceFd > 0)
    {
        close(NewNetworkNamespaceFd);
    }

    if (OriginalNetworkNamespaceFd > 0)
    {
        close(OriginalNetworkNamespaceFd);
    }

    if (DeleteDevice)
    {
        (void)DeleteVirtualDeviceViaNetlink("testbridge");
    }

    return Result;
}

int VirtualEthernetPairFromNetlink1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine creates and deletes a new virtual ethernet pair using netlink
    messages. The delete is performed on the primary device.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    bool DeleteDevice;
    int Result;

    DeleteDevice = false;

    LxtCheckResult(CreateVirtualEthernetPairViaNetlink("veth_tst0", "veth_tst1"));
    LxtCheckErrno(GetNetworkInterfaceIndex("veth_tst0"));
    LxtCheckErrno(GetNetworkInterfaceIndex("veth_tst1"));
    LxtCheckResult(DeleteVirtualDeviceViaNetlink("veth_tst0"));
    LxtCheckErrnoFailure(GetNetworkInterfaceIndex("veth_tst0"), ENODEV);
    LxtCheckErrnoFailure(GetNetworkInterfaceIndex("veth_tst1"), ENODEV);

ErrorExit:
    if (DeleteDevice)
    {
        (void)DeleteVirtualDeviceViaNetlink("veth_tst0");
    }

    return Result;
}

int VirtualEthernetPairFromNetlink2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine creates and deletes a new virtual ethernet pair using netlink
    messages. The delete is performed on the peer device.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    bool DeleteDevice;
    int Result;

    DeleteDevice = false;

    LxtCheckResult(CreateVirtualEthernetPairViaNetlink("veth_tst0", "veth_tst1"));
    DeleteDevice = true;
    LxtCheckErrno(GetNetworkInterfaceIndex("veth_tst0"));
    LxtCheckErrno(GetNetworkInterfaceIndex("veth_tst1"));
    LxtCheckResult(DeleteVirtualDeviceViaNetlink("veth_tst1"));
    DeleteDevice = false;
    LxtCheckErrnoFailure(GetNetworkInterfaceIndex("veth_tst0"), ENODEV);
    LxtCheckErrnoFailure(GetNetworkInterfaceIndex("veth_tst1"), ENODEV);

ErrorExit:
    if (DeleteDevice)
    {
        (void)DeleteVirtualDeviceViaNetlink("veth_tst1");
    }

    return Result;
}

int VirtualEthernetPairNamespace1(PLXT_ARGS Args)

/*++

Routine Description:

    This routine does basic network namespace sanity testing with a virtual
    ethernet pair:
        1) creates a new virtual ethernet pair
        2) creates a new namespace
        3) try to move one end of the pair into the other namespace

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    bool DeleteDevice;
    int NewNetworkNamespaceFd;
    int OriginalNetworkNamespaceFd;
    int Response;
    int Result;

    DeleteDevice = false;
    NewNetworkNamespaceFd = 0;
    OriginalNetworkNamespaceFd = 0;

    //
    // Open file descriptor of default network namespace.
    //

    LxtCheckErrno(OriginalNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Create a new virtual ethernet pair.
    //

    LxtCheckResult(CreateVirtualEthernetPairViaNetlink("veth0", "veth1"));
    DeleteDevice = true;

    //
    // Switch to a new network namespace.
    //

    LxtCheckErrno(unshare(CLONE_NEWNET));

    //
    // Open file descriptor of the new network namespace.
    //

    LxtCheckErrno(NewNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Switch back to original network namespace.
    //

    LxtCheckErrno(setns(OriginalNetworkNamespaceFd, CLONE_NEWNET));

    //
    // Try to move one half of the pair into the new namespace.
    //

    LxtCheckResult(SetVirtualDeviceAttributeViaNetlink("veth1", IFLA_NET_NS_FD, NewNetworkNamespaceFd, &Response));

    LxtCheckEqual(Response, 0, "%d");

    //
    // Delete the device.
    //

    LxtCheckResult(DeleteVirtualDeviceViaNetlink("veth0"));
    DeleteDevice = false;

ErrorExit:
    if (NewNetworkNamespaceFd > 0)
    {
        close(NewNetworkNamespaceFd);
    }

    if (OriginalNetworkNamespaceFd > 0)
    {
        (void)setns(OriginalNetworkNamespaceFd, CLONE_NEWNET);
        close(OriginalNetworkNamespaceFd);
    }

    if (DeleteDevice)
    {
        (void)DeleteVirtualDeviceViaNetlink("veth0");
    }

    return Result;
}

int VirtualEthernetPairNamespace2(PLXT_ARGS Args)

/*++

Routine Description:

    This routine does basic network namespace sanity testing with a virtual
    ethernet pair:
        1) create a virtual ethernet pair.
        2) create a new namespace
        3) move one end of the pair into the new namespace.
        4) create another virtual ethernet pair using the same name as that of
           the moved end.
        5) try to move the same-named end again.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    bool DeleteFirstDevice;
    bool DeleteSecondDevice;
    int NewNetworkNamespaceFd;
    int OriginalNetworkNamespaceFd;
    int Response;
    int Result;

    DeleteFirstDevice = false;
    DeleteSecondDevice = false;
    NewNetworkNamespaceFd = 0;
    OriginalNetworkNamespaceFd = 0;

    //
    // Open file descriptor of default network namespace.
    //

    LxtCheckErrno(OriginalNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Create a new virtual ethernet pair.
    //

    LxtCheckResult(CreateVirtualEthernetPairViaNetlink("veth0", "veth1"));
    DeleteFirstDevice = true;

    //
    // Switch to a new network namespace.
    //

    LxtCheckErrno(unshare(CLONE_NEWNET));

    //
    // Open file descriptor of the new network namespace.
    //

    LxtCheckErrno(NewNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Switch back to original network namespace.
    //

    LxtCheckErrno(setns(OriginalNetworkNamespaceFd, CLONE_NEWNET));

    //
    // Try to move one half of the pair into the new namespace.
    //

    LxtCheckResult(SetVirtualDeviceAttributeViaNetlink("veth1", IFLA_NET_NS_FD, NewNetworkNamespaceFd, &Response));

    LxtCheckEqual(Response, 0, "%d");

    //
    // Create a new virtual ethernet pair, re-using the moved name.
    //

    LxtCheckResult(CreateVirtualEthernetPairViaNetlink("veth2", "veth1"));
    DeleteSecondDevice = true;

    //
    // Try to move it again, expecting failure.
    //

    LxtCheckErrno(setns(OriginalNetworkNamespaceFd, CLONE_NEWNET));
    LxtCheckResult(SetVirtualDeviceAttributeViaNetlink("veth1", IFLA_NET_NS_FD, NewNetworkNamespaceFd, &Response));

    LxtCheckEqual(Response, -EEXIST, "%d");

    //
    // Cleanup.
    //

    LxtCheckResult(DeleteVirtualDeviceViaNetlink("veth0"));
    DeleteFirstDevice = false;
    LxtCheckResult(DeleteVirtualDeviceViaNetlink("veth2"));
    DeleteSecondDevice = false;

ErrorExit:
    if (NewNetworkNamespaceFd > 0)
    {
        close(NewNetworkNamespaceFd);
    }

    if (OriginalNetworkNamespaceFd > 0)
    {
        (void)setns(OriginalNetworkNamespaceFd, CLONE_NEWNET);
        close(OriginalNetworkNamespaceFd);
    }

    if (DeleteFirstDevice)
    {
        (void)DeleteVirtualDeviceViaNetlink("veth0");
    }

    if (DeleteSecondDevice)
    {
        (void)DeleteVirtualDeviceViaNetlink("veth2");
    }

    return Result;
}

int VirtualEthernetPairNamespace3(PLXT_ARGS Args)

/*++

Routine Description:

    This routine does basic network namespace sanity testing with a virtual
    ethernet pair:
        1) creates a new virtual ethernet pair
        2) creates a new namespace
        3) move one end of the pair into the other namespace
        4) close the new namespace

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    bool DeleteDevice;
    int NewNetworkNamespaceFd;
    int OriginalNetworkNamespaceFd;
    int Response;
    int Result;

    DeleteDevice = false;
    NewNetworkNamespaceFd = 0;
    OriginalNetworkNamespaceFd = 0;

    //
    // Open file descriptor of default network namespace.
    //

    LxtCheckErrno(OriginalNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Create a new virtual ethernet pair.
    //

    LxtCheckResult(CreateVirtualEthernetPairViaNetlink("veth0", "veth1"));
    DeleteDevice = true;

    //
    // Switch to a new network namespace.
    //

    LxtCheckErrno(unshare(CLONE_NEWNET));

    //
    // Open file descriptor of the new network namespace.
    //

    LxtCheckErrno(NewNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Switch back to original network namespace.
    //

    LxtCheckErrno(setns(OriginalNetworkNamespaceFd, CLONE_NEWNET));

    //
    // Try to move one half of the pair into the new namespace.
    //

    LxtCheckResult(SetVirtualDeviceAttributeViaNetlink("veth1", IFLA_NET_NS_FD, NewNetworkNamespaceFd, &Response));

    LxtCheckEqual(Response, 0, "%d");

    //
    // Close the new namespace.
    //

    LxtCheckClose(NewNetworkNamespaceFd);

    //
    // Both endpoints should have been deleted when the namespace was closed.
    // Delay a bit to let the state settle.
    //

    sleep(1);
    LxtCheckErrnoFailure(GetNetworkInterfaceIndex("veth0"), ENODEV);
    DeleteDevice = false;

ErrorExit:
    if (NewNetworkNamespaceFd > 0)
    {
        close(NewNetworkNamespaceFd);
    }

    if (OriginalNetworkNamespaceFd > 0)
    {
        (void)setns(OriginalNetworkNamespaceFd, CLONE_NEWNET);
        close(OriginalNetworkNamespaceFd);
    }

    if (DeleteDevice)
    {
        (void)DeleteVirtualDeviceViaNetlink("veth0");
    }

    return Result;
}

int VirtualEthernetPairNamespace4(PLXT_ARGS Args)

/*++

Routine Description:

    This routine does simple validation on network interface link information
    when creating virtual ethernet adapters and moving them between namespaces.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    bool DeleteDevice;
    int NewNetworkNamespaceFd;
    int OriginalNetworkNamespaceFd;
    int Response;
    int Result;

    DeleteDevice = false;
    NewNetworkNamespaceFd = 0;
    OriginalNetworkNamespaceFd = 0;

    //
    // Open file descriptor of default network namespace.
    //

    LxtCheckErrno(OriginalNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Create a new virtual ethernet pair.
    //

    LxtCheckResult(CreateVirtualEthernetPairViaNetlink("veth0", "veth1"));
    DeleteDevice = true;

    //
    // Switch to a new network namespace.
    //

    LxtCheckErrno(unshare(CLONE_NEWNET));

    //
    // Open file descriptor of the new network namespace.
    //

    LxtCheckErrno(NewNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Switch back to original network namespace.
    //

    LxtCheckErrno(setns(OriginalNetworkNamespaceFd, CLONE_NEWNET));

    //
    // Check the route link entries.
    //

    LxtCheckResult(VerifyRouteLinkExists("veth0", NULL));
    LxtCheckResult(VerifyRouteLinkExists("veth1", NULL));

    //
    // Try to move one half of the pair into the new namespace.
    //

    LxtCheckResult(SetVirtualDeviceAttributeViaNetlink("veth1", IFLA_NET_NS_FD, NewNetworkNamespaceFd, &Response));

    LxtCheckEqual(Response, 0, "%d");

    //
    // Check the route link entries.
    //

    LxtCheckErrno(setns(NewNetworkNamespaceFd, CLONE_NEWNET));
    LxtCheckResult(VerifyRouteLinkExists("veth1", NULL));
    LxtCheckErrno(setns(OriginalNetworkNamespaceFd, CLONE_NEWNET));
    LxtCheckResult(VerifyRouteLinkExists("veth0", NULL));

    //
    // Close the new namespace, and wait a second for state to settle.
    //

    LxtCheckClose(NewNetworkNamespaceFd);
    sleep(1);

    //
    // The namespace deletion should have removed the devices.
    //

    LxtCheckResult(VerifyRouteLinkDoesNotExist("veth0", NULL));
    DeleteDevice = false;
    LxtCheckResult(VerifyRouteLinkDoesNotExist("veth1", NULL));

ErrorExit:
    if (NewNetworkNamespaceFd > 0)
    {
        close(NewNetworkNamespaceFd);
    }

    if (OriginalNetworkNamespaceFd > 0)
    {
        (void)setns(OriginalNetworkNamespaceFd, CLONE_NEWNET);
        close(OriginalNetworkNamespaceFd);
    }

    if (DeleteDevice)
    {
        (void)DeleteVirtualDeviceViaNetlink("veth0");
    }

    return Result;
}

int VirtualEthernetPairNamespace5(PLXT_ARGS Args)

/*++

Routine Description:

    This routine does simple validation of socket creation being tied to the
    network namespace.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    bool DeleteDevice;
    int NewNetworkNamespaceFd;
    int OriginalNetworkNamespaceFd;
    int Response;
    int Result;
    int SocketNewNs;

    DeleteDevice = false;
    NewNetworkNamespaceFd = 0;
    OriginalNetworkNamespaceFd = 0;

    //
    // Open file descriptor of default network namespace.
    //

    LxtCheckErrno(OriginalNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Create a new virtual ethernet pair.
    //

    LxtCheckResult(CreateVirtualEthernetPairViaNetlink("veth0", "veth1"));
    DeleteDevice = true;

    //
    // Switch to a new network namespace.
    //

    LxtCheckErrno(unshare(CLONE_NEWNET));

    //
    // Create a socket while in the new network namespace.
    //

    LxtCheckErrno(SocketNewNs = socket(AF_NETLINK, SOCK_RAW, 0));

    //
    // Open file descriptor of the new network namespace.
    //

    LxtCheckErrno(NewNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Switch back to original network namespace.
    //

    LxtCheckErrno(setns(OriginalNetworkNamespaceFd, CLONE_NEWNET));

    //
    // Bind the socket created in the new network namespace.
    //

    LxtCheckErrno(BindSocketForNetlink(SocketNewNs));

    //
    // Check the route link entries.
    //

    LxtCheckResult(VerifyRouteLinkExists("veth0", NULL));
    LxtCheckResult(VerifyRouteLinkExists("veth1", NULL));

    //
    // Check the non-existence of the entries with the other socket.
    //

    LxtCheckResult(VerifyRouteLinkDoesNotExist("veth0", &SocketNewNs));
    LxtCheckResult(VerifyRouteLinkDoesNotExist("veth1", &SocketNewNs));

    //
    // Try to move one half of the pair into the new namespace.
    //

    LxtCheckResult(SetVirtualDeviceAttributeViaNetlink("veth1", IFLA_NET_NS_FD, NewNetworkNamespaceFd, &Response));

    LxtCheckEqual(Response, 0, "%d");

    //
    // Check the route link entries.
    //

    LxtCheckResult(VerifyRouteLinkExists("veth0", NULL));
    LxtCheckResult(VerifyRouteLinkDoesNotExist("veth0", &SocketNewNs));
    LxtCheckResult(VerifyRouteLinkDoesNotExist("veth1", NULL));
    LxtCheckResult(VerifyRouteLinkExists("veth1", &SocketNewNs));

    //
    // Close the new namespace and the related socket, and wait a second for
    // state to settle.
    //

    LxtCheckClose(SocketNewNs);
    SocketNewNs = 0;
    LxtCheckClose(NewNetworkNamespaceFd);
    sleep(1);

    //
    // The namespace deletion should have removed the devices.
    //

    LxtCheckResult(VerifyRouteLinkDoesNotExist("veth0", NULL));
    DeleteDevice = false;
    LxtCheckResult(VerifyRouteLinkDoesNotExist("veth1", NULL));

ErrorExit:
    if (SocketNewNs > 0)
    {
        close(SocketNewNs);
    }

    if (NewNetworkNamespaceFd > 0)
    {
        close(NewNetworkNamespaceFd);
    }

    if (OriginalNetworkNamespaceFd > 0)
    {
        (void)setns(OriginalNetworkNamespaceFd, CLONE_NEWNET);
        close(OriginalNetworkNamespaceFd);
    }

    if (DeleteDevice)
    {
        (void)DeleteVirtualDeviceViaNetlink("veth0");
    }

    return Result;
}

int VirtualEthernetPairConfigure(PLXT_ARGS Args)

/*++

Routine Description:

    This routine creates a virtual ethernet pair and attempts to bring them up
    and assign them static IP addresses.

Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    bool DeleteDevice;
    struct in_addr AddressIpv4;
    int Response;
    int Result;

    DeleteDevice = false;

    //
    // Create a new virtual ethernet pair.
    //

    LxtCheckResult(CreateVirtualEthernetPairViaNetlink("veth0", "veth1"));
    DeleteDevice = true;

    //
    // Bring the interfaces up and assign static IP addresses.
    //

    LxtCheckResult(SetVirtualDeviceFlagViaNetlink("veth0", IFF_UP, true, &Response));

    LxtCheckEqual(Response, 0, "%d");
    inet_aton(LXT_IP_ADDRESS1, &AddressIpv4);
    LxtCheckResult(SetIpAddress("veth0", &AddressIpv4, 32));

    LxtCheckResult(SetVirtualDeviceFlagViaNetlink("veth1", IFF_UP, true, &Response));

    LxtCheckEqual(Response, 0, "%d");
    inet_aton(LXT_IP_ADDRESS2, &AddressIpv4);
    LxtCheckResult(SetIpAddress("veth1", &AddressIpv4, 32));

    //
    // Try to delete the device.
    //

    LxtCheckResult(DeleteVirtualDeviceViaNetlink("veth0"));
    DeleteDevice = false;

ErrorExit:
    if (DeleteDevice)
    {
        (void)DeleteVirtualDeviceViaNetlink("veth0");
    }

    return Result;
}

int VirtualEthernetPairData(PLXT_ARGS Args)

/*++

Routine Description:


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct sockaddr_in AddressIpv4 = {};
    socklen_t AddressLength;
    ssize_t BytesReceived;
    ssize_t BytesSent;
    bool DeleteDevice;
    struct sockaddr_in FromAddressIpv4;
    ;
    char RecvBuffer[10] = {0};
    int Response;
    int Result;
    char SendBuffer[10] = {"123456789"};
    int Socket;
    int Socket2;

    DeleteDevice = false;
    Socket = -1;
    Socket2 = -1;

    //
    // Make sure the loopback adapter is up.
    //

    (void)SetVirtualDeviceFlagViaNetlink("lo", IFF_UP, true, &Response);

    //
    // Create a new virtual ethernet pair.
    //

    LxtCheckResult(CreateVirtualEthernetPairViaNetlink("veth0", "veth1"));
    DeleteDevice = true;

    //
    // Bring the interfaces up and assign static IP addresses.
    //

    LxtCheckResult(SetVirtualDeviceFlagViaNetlink("veth0", IFF_UP, true, &Response));

    LxtCheckEqual(Response, 0, "%d");
    AddressIpv4.sin_family = AF_INET;
    inet_aton(LXT_IP_ADDRESS1, &AddressIpv4.sin_addr);
    LxtCheckResult(SetIpAddress("veth0", &AddressIpv4.sin_addr, 32));
    LxtCheckErrno((Socket = socket(AF_INET, SOCK_DGRAM, 0)));
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&AddressIpv4, sizeof(AddressIpv4)));

    LxtCheckResult(SetVirtualDeviceFlagViaNetlink("veth1", IFF_UP, true, &Response));

    LxtCheckEqual(Response, 0, "%d");
    LxtCheckResult(SetRoute("veth1", &AddressIpv4.sin_addr, 32));
    inet_aton(LXT_IP_ADDRESS2, &AddressIpv4.sin_addr);
    LxtCheckResult(SetIpAddress("veth1", &AddressIpv4.sin_addr, 32));
    LxtCheckErrno((Socket2 = socket(AF_INET, SOCK_DGRAM, 0)));
    LxtCheckErrno(bind(Socket2, (struct sockaddr*)&AddressIpv4, sizeof(AddressIpv4)));

    //
    // Send a packet between the devices.
    //

    AddressLength = sizeof(AddressIpv4);
    LxtCheckErrno(getsockname(Socket2, (struct sockaddr*)&AddressIpv4, &AddressLength));

    LxtCheckEqual(AddressLength, sizeof(AddressIpv4), "%lu");
    LxtCheckResult(SetRoute("veth0", &AddressIpv4.sin_addr, 32));
    LxtLogInfo("AddressIpv4.sin_family = %d", AddressIpv4.sin_family);
    LxtLogInfo("AddressIpv4.sin_port = %d", AddressIpv4.sin_port);
    LxtLogInfo("AddressIpv4.sin_addr = %08x", AddressIpv4.sin_addr.s_addr);
    LxtCheckErrno((BytesSent = sendto(Socket, SendBuffer, sizeof(SendBuffer), 0, (struct sockaddr*)&AddressIpv4, sizeof(AddressIpv4))));

    LxtCheckEqual(BytesSent, sizeof(SendBuffer), "%lu");
    AddressLength = sizeof(FromAddressIpv4);
    LxtCheckErrno((BytesReceived = recvfrom(Socket2, RecvBuffer, sizeof(RecvBuffer), 0, (struct sockaddr*)&FromAddressIpv4, &AddressLength)));

    LxtCheckEqual(BytesReceived, sizeof(SendBuffer), "%lu");
    LxtCheckMemoryEqual(RecvBuffer, SendBuffer, BytesReceived);
    LxtCheckEqual(AddressLength, sizeof(FromAddressIpv4), "%lu");

    //
    // Try to delete the device.
    //

    LxtCheckResult(DeleteVirtualDeviceViaNetlink("veth0"));
    DeleteDevice = false;

ErrorExit:
    if (Socket > 0)
    {
        close(Socket);
    }

    if (Socket2 > 0)
    {
        close(Socket2);
    }

    if (DeleteDevice)
    {
        (void)DeleteVirtualDeviceViaNetlink("veth0");
    }

    return Result;
}

int VirtualEthernetPairNamespaceData(PLXT_ARGS Args)

/*++

Routine Description:


Arguments:

    Args - Supplies the command line arguments.

Return Value:

    Returns 0 on success, -1 on failure.

--*/

{

    struct sockaddr_in AddressIpv4 = {};
    socklen_t AddressLength;
    ssize_t BytesReceived;
    ssize_t BytesSent;
    bool DeleteDevice;
    struct sockaddr_in FromAddressIpv4;
    ;
    int NewNetworkNamespaceFd;
    int OriginalNetworkNamespaceFd;
    char RecvBuffer[10] = {0};
    int Response;
    int Result;
    char SendBuffer[10] = {"123456789"};
    int Socket;
    int Socket2;

    DeleteDevice = false;
    NewNetworkNamespaceFd = 0;
    OriginalNetworkNamespaceFd = 0;
    Socket = -1;
    Socket2 = -1;

    //
    // Open file descriptor of default network namespace.
    //

    LxtCheckErrno(OriginalNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Switch to a new network namespace.
    //

    LxtCheckErrno(unshare(CLONE_NEWNET));

    //
    // Open file descriptor of the new network namespace.
    //

    LxtCheckErrno(NewNetworkNamespaceFd = open("/proc/self/ns/net", 0));

    //
    // Switch back to original network namespace.
    //

    LxtCheckErrno(setns(OriginalNetworkNamespaceFd, CLONE_NEWNET));

    //
    // Create a new virtual ethernet pair.
    //

    LxtCheckResult(CreateVirtualEthernetPairViaNetlink("veth0", "veth1"));
    DeleteDevice = true;

    //
    // Try to move one half of the pair into the new namespace.
    //

    LxtCheckResult(SetVirtualDeviceAttributeViaNetlink("veth1", IFLA_NET_NS_FD, NewNetworkNamespaceFd, &Response));

    LxtCheckEqual(Response, 0, "%d");

    //
    // Bring the interfaces up and assign static IP addresses.
    //

    LxtCheckResult(SetVirtualDeviceFlagViaNetlink("veth0", IFF_UP, true, &Response));

    LxtCheckEqual(Response, 0, "%d");
    AddressIpv4.sin_family = AF_INET;
    inet_aton(LXT_IP_ADDRESS1, &AddressIpv4.sin_addr);
    LxtCheckResult(SetIpAddress("veth0", &AddressIpv4.sin_addr, 32));
    LxtCheckErrno((Socket = socket(AF_INET, SOCK_DGRAM, 0)));
    LxtCheckErrno(bind(Socket, (struct sockaddr*)&AddressIpv4, sizeof(AddressIpv4)));

    //
    // Switch to the new network namespace to configure the other endpoint.
    //

    LxtCheckErrno(setns(NewNetworkNamespaceFd, CLONE_NEWNET));
    LxtCheckResult(SetVirtualDeviceFlagViaNetlink("veth1", IFF_UP, true, &Response));

    LxtCheckEqual(Response, 0, "%d");
    LxtCheckResult(SetRoute("veth1", &AddressIpv4.sin_addr, 32));
    inet_aton(LXT_IP_ADDRESS2, &AddressIpv4.sin_addr);
    LxtCheckResult(SetIpAddress("veth1", &AddressIpv4.sin_addr, 32));

    //
    // Open UDP sockets to try to send and receive on the IP addresses assigned
    // above.
    //

    LxtCheckErrno((Socket2 = socket(AF_INET, SOCK_DGRAM, 0)));
    LxtCheckErrno(bind(Socket2, (struct sockaddr*)&AddressIpv4, sizeof(AddressIpv4)));

    AddressLength = sizeof(AddressIpv4);
    LxtCheckErrno(getsockname(Socket2, (struct sockaddr*)&AddressIpv4, &AddressLength));

    LxtCheckEqual(AddressLength, sizeof(AddressIpv4), "%lu");

    //
    // Switch back to original network namespace.
    //

    LxtCheckErrno(setns(OriginalNetworkNamespaceFd, CLONE_NEWNET));

    //
    // Send a packet between the devices.
    //

    LxtCheckResult(SetRoute("veth0", &AddressIpv4.sin_addr, 32));
    LxtLogInfo("AddressIpv4.sin_family = %d", AddressIpv4.sin_family);
    LxtLogInfo("AddressIpv4.sin_port = %d", AddressIpv4.sin_port);
    LxtLogInfo("AddressIpv4.sin_addr = %08x", AddressIpv4.sin_addr.s_addr);
    LxtCheckErrno((BytesSent = sendto(Socket, SendBuffer, sizeof(SendBuffer), 0, (struct sockaddr*)&AddressIpv4, sizeof(AddressIpv4))));

    LxtCheckEqual(BytesSent, sizeof(SendBuffer), "%lu");
    AddressLength = sizeof(FromAddressIpv4);
    LxtCheckErrno((BytesReceived = recvfrom(Socket2, RecvBuffer, sizeof(RecvBuffer), 0, (struct sockaddr*)&FromAddressIpv4, &AddressLength)));

    LxtCheckEqual(BytesReceived, sizeof(SendBuffer), "%lu");
    LxtCheckMemoryEqual(RecvBuffer, SendBuffer, BytesReceived);
    LxtCheckEqual(AddressLength, sizeof(FromAddressIpv4), "%lu");

    //
    // Try to delete the device.
    //

    LxtCheckResult(DeleteVirtualDeviceViaNetlink("veth0"));
    DeleteDevice = false;

ErrorExit:
    if (NewNetworkNamespaceFd > 0)
    {
        close(NewNetworkNamespaceFd);
    }

    if (OriginalNetworkNamespaceFd > 0)
    {
        (void)setns(OriginalNetworkNamespaceFd, CLONE_NEWNET);
        close(OriginalNetworkNamespaceFd);
    }

    if (Socket > 0)
    {
        close(Socket);
    }

    if (Socket2 > 0)
    {
        close(Socket2);
    }

    if (DeleteDevice)
    {
        (void)DeleteVirtualDeviceViaNetlink("veth0");
    }

    return Result;
}
