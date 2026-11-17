// Copyright (C) Microsoft Corporation. All rights reserved.

#include "common.h"
#include "securityaudit/NetworkAudit.h"
#include <arpa/inet.h>
#include <netpacket/packet.h>
#include <sys/syscall.h>
#include <linux/types.h>
#include "event_defs.h"

namespace wsl::linux::securityaudit {
namespace {
    std::optional<LX_NETWORK_AUDIT_OPERATION> NetworkOperation(unsigned long syscall)
    {
        switch (syscall)
        {
        case __NR_socket:
            return LX_NETWORK_AUDIT_OPERATION::Socket;
        case __NR_connect:
            return LX_NETWORK_AUDIT_OPERATION::Connect;
        case __NR_bind:
            return LX_NETWORK_AUDIT_OPERATION::Bind;
        case __NR_accept:
            return LX_NETWORK_AUDIT_OPERATION::Accept;
        case __NR_accept4:
            return LX_NETWORK_AUDIT_OPERATION::Accept4;
        default:
            return {};
        }
    }

    LX_AUDIT_ENDPOINT NetworkSockaddr(const event_socket_s& event, bool includePort, bool inputAddress)
    {
        LX_AUDIT_ENDPOINT endpoint{};
        if (event.base.status & (STATUS_VALUE | STATUS_NOARGS))
        {
            LOG_ERROR("Security audit omitted an unavailable network sockaddr");
            return endpoint;
        }

        const auto family = event.socket_op.addr.sin_family;
        const auto size = family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        // The producer reads fixed-size sockaddrs, even when a failed call supplied a shorter buffer.
        if (inputAddress && (event.base.a[1] == 0 || static_cast<int32_t>(event.base.a[2]) < static_cast<int32_t>(size)))
        {
            LOG_ERROR("Security audit omitted a short network sockaddr");
            return endpoint;
        }
        if (family == AF_INET)
        {
            endpoint.Family = 4;
            memcpy(endpoint.Address, &event.socket_op.addr.sin_addr, sizeof(in_addr));
            if (includePort)
            {
                endpoint.Port = ntohs(event.socket_op.addr.sin_port);
            }
        }
        else if (family == AF_INET6)
        {
            endpoint.Family = 6;
            memcpy(endpoint.Address, &event.socket_op.addr6.sin6_addr, sizeof(in6_addr));
            if (includePort)
            {
                endpoint.Port = ntohs(event.socket_op.addr6.sin6_port);
            }
        }
        return endpoint;
    }

    LX_AUDIT_ENDPOINT NetworkLocalEndpoint(const eventop_socket_s& socket)
    {
        LX_AUDIT_ENDPOINT endpoint{};
        if (socket.local_ip_family == AF_INET)
        {
            endpoint.Family = 4;
            memcpy(endpoint.Address, &socket.local_addr4, sizeof(socket.local_addr4));
        }
        else if (socket.local_ip_family == AF_INET6)
        {
            endpoint.Family = 6;
            memcpy(endpoint.Address, &socket.local_addr6, sizeof(socket.local_addr6));
        }
        if (endpoint.Family != 0)
        {
            // Unlike sockaddr ports, the SDK's skc_num-derived local port is already in host byte order.
            endpoint.Port = socket.local_port;
        }
        return endpoint;
    }
} // namespace

std::optional<LX_INIT_NETWORK_AUDIT> DecodeNetworkAudit(gsl::span<const gsl::byte> data)
{
    THROW_ERRNO_IF(EINVAL, data.size() < sizeof(event_begin_s));
    event_begin_s header{};
    memcpy(&header, data.data(), sizeof(header));
    if (header.event_type != event_type_socket && header.event_type != event_type_regular)
    {
        return {};
    }

    constexpr auto c_syscallOffset = sizeof(event_begin_s) + offsetof(event_base_s, syscall_id);
    unsigned long syscall{};
    THROW_ERRNO_IF(EINVAL, data.size() < c_syscallOffset + sizeof(syscall));
    memcpy(&syscall, data.data() + c_syscallOffset, sizeof(syscall));
    const auto operation = NetworkOperation(syscall);
    if (!operation)
    {
        return {};
    }

    event_socket_s event{};
    THROW_ERRNO_IF(EINVAL, data.size() != sizeof(event) || header.size != sizeof(event) || header.event_type != event_type_socket);
    memcpy(&event, data.data(), sizeof(event));
    THROW_ERRNO_IF(EINVAL, event.end.code_bytes_end != CODE_BYTES);

    LX_INIT_NETWORK_AUDIT message{};
    message.Header.MessageType = message.Type;
    message.Header.MessageSize = sizeof(message);
    message.Operation = *operation;
    message.Uid = message.Euid = INVALID_CRED;
    message.Pid = event.base.pid;
    if (!(event.base.status & STATUS_CRED))
    {
        message.Uid = event.base.uid;
        if (event.base.enriched == 1)
        {
            message.Euid = event.base.euid;
        }
    }
    else
    {
        LOG_ERROR("Security audit omitted unavailable network credentials");
    }
    if (event.base.enriched == 1)
    {
        message.PidNamespace = event.base.pid_ns_id;
        message.MountNamespace = event.base.mnt_ns_id;
        message.NetworkNamespace = event.base.net_ns_id;
    }
    else
    {
        LOG_ERROR("Security audit omitted unavailable network enrichment");
    }
    if (!(event.base.status & STATUS_RC) && event.base.return_code >= -4095)
    {
        message.Fields |= LxNetworkAuditResult;
        message.Result = event.base.return_code;
    }
    else
    {
        LOG_ERROR("Security audit omitted an unavailable network syscall result");
    }

    switch (*operation)
    {
    case LX_NETWORK_AUDIT_OPERATION::Socket:
        if (!(event.base.status & STATUS_NOARGS))
        {
            // This is the requested protocol, including an explicit zero; do not infer TCP/UDP from socket type.
            message.Protocol = static_cast<int32_t>(event.base.a[2]);
            message.Fields |= LxNetworkAuditProtocol;
        }
        else
        {
            LOG_ERROR("Security audit omitted an unavailable socket protocol argument");
        }
        break;
    case LX_NETWORK_AUDIT_OPERATION::Bind:
        message.Local = NetworkSockaddr(event, false, true);
        break;
    case LX_NETWORK_AUDIT_OPERATION::Connect:
        message.Remote = NetworkSockaddr(event, true, true);
        // The SDK only enriches local socket state after a nonnegative connect result, not EINPROGRESS.
        if ((message.Fields & LxNetworkAuditResult) && message.Result >= 0)
        {
            message.Local = NetworkLocalEndpoint(event.socket_op);
        }
        break;
    case LX_NETWORK_AUDIT_OPERATION::Accept:
    case LX_NETWORK_AUDIT_OPERATION::Accept4:
        // Failed accept can still contain the producer's legacy user-buffer fallback, not an accepted peer.
        if ((message.Fields & LxNetworkAuditResult) && message.Result >= 0)
        {
            message.Local = NetworkLocalEndpoint(event.socket_op);
            message.Remote = NetworkSockaddr(event, true, false);
        }
        break;
    default:
        break;
    }
    return message;
}

} // namespace wsl::linux::securityaudit
