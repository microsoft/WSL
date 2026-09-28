/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    conncheckshared.h

Abstract:

    This file contains a simple network connectivity check shared between Windows and Linux.
--*/

#pragma once

#if defined(_MSC_VER)
// Windows
#include <winsock2.h>
#include <windows.h>
#define ConnCheckGetLastError() WSAGetLastError()
#define CONNCHECK_ERROR_PENDING WSAEWOULDBLOCK
#else
// Linux
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <wchar.h>
#define ConnCheckGetLastError() errno
#define CONNCHECK_ERROR_PENDING EINPROGRESS
#endif

namespace wsl::shared::conncheck {

#if defined(_MSC_VER)
using unique_socket = wil::unique_socket;
#else
using unique_socket = wil::unique_fd;
#endif

enum class ConnCheckStatus
{
    InProgress,
    Success,
    FailureGetAddrInfo,
    FailureConfig,
    FailureSocketConnect,
};

struct ConnCheckResult
{
    ConnCheckStatus Ipv4Status{};
    ConnCheckStatus Ipv6Status{};
};

inline unique_socket ConnCheckConfigureSocket(int family, int socktype, int protocol)
{
    unique_socket sock{::socket(family, socktype, protocol)};
    if (!sock)
    {
        throw std::runtime_error(std::format("CheckConnection: socket() failed: {}", ConnCheckGetLastError()));
    }

#if defined(_MSC_VER)
    unsigned long value = 1;
    const int status = ::ioctlsocket(sock.get(), FIONBIO, &value);
    if (status != 0)
    {
        throw std::runtime_error(std::format("CheckConnection: ioctlsocket(FIONBIO) failed: {}", ConnCheckGetLastError()));
    }
#else
    int value = fcntl(sock.get(), F_GETFL, 0);
    if (value < 0)
    {
        throw std::runtime_error(std::format("CheckConnection: fcntl(F_GETFL) failed: {}", ConnCheckGetLastError()));
    }
    value = fcntl(sock.get(), F_SETFL, value | O_NONBLOCK);
    if (value < 0)
    {
        throw std::runtime_error(std::format("CheckConnection: fcntl(F_SETFL) failed: {}", ConnCheckGetLastError()));
    }
#endif

    return sock;
}

inline unique_socket ConnCheckConnectSocket(int family, const char* hostname, const char* port, ConnCheckResult* resultStatus) noexcept
{
    // update the ConnCheckStatus as we attempt to connect
    ConnCheckStatus* connCheckStatus = (family == AF_INET) ? &(resultStatus->Ipv4Status) : &(resultStatus->Ipv6Status);
    unique_socket sock;
    try
    {
        // first step, try to resolve the name
        *connCheckStatus = ConnCheckStatus::FailureGetAddrInfo;

        printf("CheckConnection: resolving the name %s [%s]\n", hostname, (family == AF_INET) ? "AF_INET" : "AF_INET6");
        addrinfo* servinfo = nullptr;
        const auto freeAddrInfoOnExit = wil::scope_exit([&] {
            if (servinfo)
            {
                freeaddrinfo(servinfo);
            }
        });

        addrinfo hints{};
        hints.ai_family = family;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICSERV;
        auto status = getaddrinfo(hostname, port, &hints, &servinfo);
        if (status != 0)
        {
            throw std::runtime_error(std::format("CheckConnection: getaddrinfo() failed: {}", status));
        }

        // next configure the socket
        *connCheckStatus = ConnCheckStatus::FailureConfig;
        sock = ConnCheckConfigureSocket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);

        const void* pAddr = (AF_INET == family)
                                ? static_cast<const void*>(&(reinterpret_cast<sockaddr_in*>(servinfo->ai_addr))->sin_addr)
                                : static_cast<const void*>(&(reinterpret_cast<sockaddr_in6*>(servinfo->ai_addr))->sin6_addr);
        char dst[INET6_ADDRSTRLEN * 2]{};
        printf("CheckConnection: connecting to %s\n", inet_ntop(servinfo->ai_family, pAddr, dst, sizeof(dst)));

        // next connect the socket
        *connCheckStatus = ConnCheckStatus::FailureSocketConnect;
        status = connect(sock.get(), servinfo->ai_addr, static_cast<int>(servinfo->ai_addrlen));
        if (status != 0 && ConnCheckGetLastError() != CONNCHECK_ERROR_PENDING)
        {
            throw std::runtime_error(std::format("CheckConnection: connect() failed: {}", ConnCheckGetLastError()));
        }

        // success
        *connCheckStatus = ConnCheckStatus::InProgress;
    }
    CATCH_LOG()

    return sock;
}

// Attempts to establish a TCPv4 and a TCPv6 connection to a port on a host.
// ipv6hostname is an optional parameter in case the IPv6 equivalent hostname is different.
//     example: www.msftconnecttest.com and ipv6.msftconnecttest.com
// This API is blocking/synchronous.
inline ConnCheckResult CheckConnection(const char* hostname, const char* ipv6hostname, const char* port)
{
    ConnCheckResult result{};
    result.Ipv4Status = ConnCheckStatus::InProgress;
    result.Ipv6Status = ConnCheckStatus::InProgress;

    if (ipv6hostname == nullptr)
    {
        ipv6hostname = hostname;
    }

    const auto v4sock = ConnCheckConnectSocket(AF_INET, hostname, port, &result);
    const auto v6sock = ConnCheckConnectSocket(AF_INET6, ipv6hostname, port, &result);
    const auto startTime = std::chrono::steady_clock::now();

    while (result.Ipv4Status == ConnCheckStatus::InProgress || result.Ipv6Status == ConnCheckStatus::InProgress)
    {
        constexpr long maxMillisecondsElapsed = 5000;
        const auto currentTime = std::chrono::steady_clock::now();
        const auto elapsedTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime);
        if (elapsedTimeMs.count() >= maxMillisecondsElapsed)
        {
            if (result.Ipv4Status == ConnCheckStatus::InProgress)
            {
                wprintf(L"CheckConnection: result.Ipv4Status = ConnCheckStatus::FailureSocketConnect\n");
                result.Ipv4Status = ConnCheckStatus::FailureSocketConnect;
            }
            if (result.Ipv6Status == ConnCheckStatus::InProgress)
            {
                wprintf(L"CheckConnection: result.Ipv6Status = ConnCheckStatus::FailureSocketConnect\n");
                result.Ipv6Status = ConnCheckStatus::FailureSocketConnect;
            }
            // bailing if we have timed out
            break;
        }

        fd_set writeSocketSet{};
        if (result.Ipv4Status == ConnCheckStatus::InProgress)
        {
            FD_SET(v4sock.get(), &writeSocketSet);
        }
        if (result.Ipv6Status == ConnCheckStatus::InProgress)
        {
            FD_SET(v6sock.get(), &writeSocketSet);
        }

        const int maxsocket = std::max(
            (result.Ipv4Status == ConnCheckStatus::InProgress) ? (int)v4sock.get() : -1,
            (result.Ipv6Status == ConnCheckStatus::InProgress) ? (int)v6sock.get() : -1);
        if (maxsocket != -1)
        {
            timeval timeout{};
            timeout.tv_sec = static_cast<long>(maxMillisecondsElapsed - elapsedTimeMs.count()) / 1000;
            wprintf(L"CheckConnection: arming select for %d seconds\n", timeout.tv_sec);
            const auto status = select(maxsocket + 1, nullptr, &writeSocketSet, nullptr, &timeout);
            if (status == 0)
            {
                if (result.Ipv4Status == ConnCheckStatus::InProgress)
                {
                    wprintf(L"CheckConnection: result.Ipv4Status = ConnCheckStatus::FailureSocketConnect (timeout)\n");
                    result.Ipv4Status = ConnCheckStatus::FailureSocketConnect;
                }
                if (result.Ipv6Status == ConnCheckStatus::InProgress)
                {
                    wprintf(L"CheckConnection: result.Ipv6Status = ConnCheckStatus::FailureSocketConnect (timeout)\n");
                    result.Ipv6Status = ConnCheckStatus::FailureSocketConnect;
                }
            }
            else if (status < 0)
            {
                const auto error = errno;
                if (result.Ipv4Status == ConnCheckStatus::InProgress)
                {
                    wprintf(L"CheckConnection: result.Ipv4Status = ConnCheckStatus::FailureSocketConnect (%d)\n", error);
                    result.Ipv4Status = ConnCheckStatus::FailureSocketConnect;
                }
                if (result.Ipv6Status == ConnCheckStatus::InProgress)
                {
                    wprintf(L"CheckConnection: result.Ipv6Status = ConnCheckStatus::FailureSocketConnect (%d)\n", error);
                    result.Ipv6Status = ConnCheckStatus::FailureSocketConnect;
                }
            }
            else
            {
                // Success.
                if (v4sock)
                {
                    if (FD_ISSET(v4sock.get(), &writeSocketSet))
                    {
                        wprintf(L"CheckConnection: v4 succeeded\n");
                        result.Ipv4Status = ConnCheckStatus::Success;
                    }
                }

                if (v6sock)
                {
                    if (FD_ISSET(v6sock.get(), &writeSocketSet))
                    {
                        wprintf(L"CheckConnection: v6 succeeded\n");
                        result.Ipv6Status = ConnCheckStatus::Success;
                    }
                }
            }
        }
    }

    wprintf(L"CheckConnection: returning v4 (%d) v6 (%d)\n", result.Ipv4Status, result.Ipv6Status);
    return result;
}
} // namespace wsl::shared::conncheck