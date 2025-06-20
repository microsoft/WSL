// Copyright (C) Microsoft Corporation. All rights reserved.

#include <filesystem>
#include <optional>
#include <regex>
#include <iostream>
#include <linux/audit.h> /* Definition of AUDIT_* constants */
#include <linux/sock_diag.h>
#include <linux/inet_diag.h>
#include <linux/net.h>
#include <sys/xattr.h>
#include "common.h" // Needs to be included before sal.h before of __reserved macro
#include "NetlinkTransactionError.h"
#include "GnsPortTracker.h"
#include "lxinitshared.h"

constexpr size_t c_bind_timeout_seconds = 60;
constexpr auto c_sock_diag_refresh_delay = std::chrono::milliseconds(500);
constexpr auto c_sock_diag_poll_timeout = std::chrono::milliseconds(10);
constexpr auto c_bpf_poll_timeout = std::chrono::milliseconds(500);

GnsPortTracker::GnsPortTracker(
    std::shared_ptr<wsl::shared::SocketChannel> hvSocketChannel, NetlinkChannel&& netlinkChannel, std::shared_ptr<SecCompDispatcher> seccompDispatcher) :
    m_hvSocketChannel(std::move(hvSocketChannel)), m_channel(std::move(netlinkChannel)), m_seccompDispatcher(seccompDispatcher)
{
    m_networkNamespace = std::filesystem::read_symlink("/proc/self/ns/net").string();
}

void GnsPortTracker::RunPortRefresh()
{
    UtilSetThreadName("GnsPortTracker");

    // The polling of bound sockets is done in a separate thread because
    // sock_diag sometimes fails with EBUSY when a bind() is in progress.
    // Doing this in a separate thread allows the main thread not to be delayed
    // because of transient sock_diag failures

    for (;;)
    {
        // Netlink will sometimes return EBUSY. Don't fail for that
        try
        {
            std::promise<void> resume;
            auto result = PortRefreshResult{ListAllocatedPorts(), time(nullptr), std::bind(&std::promise<void>::set_value, &resume)};
            m_allocatedPortsRefresh.set_value(result);

            resume.get_future().wait();
        }
        catch (const NetlinkTransactionError& e)
        {
            if (e.Error().value_or(0) != -EBUSY)
            {
                std::cerr << "Failed to refresh allocated ports, " << e.what() << std::endl;
            }
        }

        std::this_thread::sleep_for(c_sock_diag_refresh_delay);
    }
}

int GnsPortTracker::ProcessSecCompNotification(seccomp_notif* notification)
{
    seccomp_notif notificationCopy = *notification;
    m_request.post(notificationCopy);
    return m_reply.get();
}

void GnsPortTracker::Run()
{
    // This method consumes seccomp notifications and allows / disallows port allocations
    // depending on wsl core's response.
    // After dealing with a notification it also looks at the bound ports list to check
    // for port deallocation

    std::thread{std::bind(&GnsPortTracker::RunPortRefresh, this)}.detach();

    auto future = std::make_optional(m_allocatedPortsRefresh.get_future());
    std::optional<PortRefreshResult> refreshResult;

    for (;;)
    {
        std::optional<BindCall> bindCall;
        try
        {
            bindCall = ReadNextRequest();
        }
        catch (const std::exception& e)
        {
            GNS_LOG_ERROR("Failed to read bind request, {}", e.what());
        }

        if (bindCall.has_value())
        {
            int result = 0;
            if (bindCall->Request.has_value())
            {
                PortAllocation& allocationRequest = bindCall->Request.value();
                result = HandleRequest(allocationRequest);
                if (result == 0)
                {
                    m_allocatedPorts.emplace(std::make_pair(allocationRequest, std::make_optional(time(nullptr) + c_bind_timeout_seconds)));
                    GNS_LOG_INFO(
                        "Tracking bind call: family ({}) port ({}) protocol ({})",
                        allocationRequest.Family,
                        allocationRequest.Port,
                        allocationRequest.Protocol);
                }
            }

            try
            {
                CompleteRequest(bindCall->CallId, result);
            }
            catch (const std::exception& e)
            {
                GNS_LOG_ERROR("Failed to complete bind request, {}", e.what());
            }
        }

        // If bindCall is empty, then the read() timed out. Look for any closed port
        if (future.has_value() && future->wait_for(c_sock_diag_poll_timeout) == std::future_status::ready)
        {
            refreshResult.emplace(future->get());
            future.reset();
            m_allocatedPortsRefresh = {};

            // If this loop's iteration had a bind call, it's possible that RefreshAllocatedPort
            // was called before the bind called was processed. Make sure that the port list
            // is up to date (If this is called, the next block will schedule another refresh)
            if (!bindCall.has_value())
            {
                OnRefreshAllocatedPorts(refreshResult->Ports, refreshResult->Timestamp);
            }
        }

        // Only look at bound ports if there's something to deallocate to avoid wasting cycles
        if (refreshResult.has_value() && !m_allocatedPorts.empty())
        {
            future = m_allocatedPortsRefresh.get_future();
            refreshResult->Resume(); // This will resume the sock_diag thread
            refreshResult.reset();
        }
    }
}

std::set<GnsPortTracker::PortAllocation> GnsPortTracker::ListAllocatedPorts()
{
    std::set<PortAllocation> ports;

    inet_diag_req_v2 message{};
    message.sdiag_family = AF_INET;
    message.sdiag_protocol = IPPROTO_TCP;
    message.idiag_states = ~0;

    auto onMessage = [&](const NetlinkResponse& response) {
        for (const auto& e : response.Messages<inet_diag_msg>(SOCK_DIAG_BY_FAMILY))
        {
            const auto* payload = e.Payload();
            in6_addr address = {};

            if (payload->idiag_family == AF_INET6)
            {
                static_assert(sizeof(address.s6_addr32) == 16);
                static_assert(sizeof(address.s6_addr32) == sizeof(payload->id.idiag_src));
                memcpy(address.s6_addr32, payload->id.idiag_src, sizeof(address.s6_addr32));
            }
            else
            {
                address.s6_addr32[0] = payload->id.idiag_src[0];
            }

            ports.emplace(ntohs(payload->id.idiag_sport), static_cast<int>(payload->idiag_family), static_cast<int>(message.sdiag_protocol), address);
        }
    };

    {
        auto transaction = m_channel.CreateTransaction(message, SOCK_DIAG_BY_FAMILY, NLM_F_DUMP);
        transaction.Execute(onMessage);
    }

    message.sdiag_family = AF_INET6;
    {
        auto transaction = m_channel.CreateTransaction(message, SOCK_DIAG_BY_FAMILY, NLM_F_DUMP);
        transaction.Execute(onMessage);
    }

    message.sdiag_protocol = IPPROTO_UDP;
    message.sdiag_family = AF_INET;
    {
        auto transaction = m_channel.CreateTransaction(message, SOCK_DIAG_BY_FAMILY, NLM_F_DUMP);
        transaction.Execute(onMessage);
    }

    message.sdiag_family = AF_INET6;
    {
        auto transaction = m_channel.CreateTransaction(message, SOCK_DIAG_BY_FAMILY, NLM_F_DUMP);
        transaction.Execute(onMessage);
    }

    return ports;
}

void GnsPortTracker::OnRefreshAllocatedPorts(const std::set<PortAllocation>& Ports, time_t Timestamp)
{
    // Because there's no way to get notified when the bind() call actually completes, it' possible
    // that this method is called before the bind() completion and so the port allocation may not be visible yet.
    // To avoid deallocating ports that simply haven't been done allocating yet, m_allocatedPorts stores a timeout
    // that prevents deallocating the port unless:
    //
    // - The port has been seen to be allocated (if so, then the timeout is empty)
    // - The timeout has expired

    for (auto it = m_allocatedPorts.begin(); it != m_allocatedPorts.end();)
    {
        if (Ports.find(it->first) == Ports.end())
        {
            if (!it->second.has_value() || it->second.value() < Timestamp)
            {
                auto result = RequestPort(it->first, false);
                if (result != 0)
                {
                    std::cerr << "GnsPortTracker: Failed to deallocate port " << it->first << ", " << result << std::endl;
                }

                GNS_LOG_INFO(
                    "No longer tracking bind call: family ({}) port ({}) protocol ({})",
                    it->first.Family,
                    it->first.Port,
                    it->first.Protocol);
                it = m_allocatedPorts.erase(it);
                continue;
            }
        }
        else
        {
            it->second.reset(); // The port is known to be allocated, remove the timeout
        }

        it++;
    }
}

int GnsPortTracker::RequestPort(const PortAllocation& Port, bool allocate)
{
    LX_GNS_PORT_ALLOCATION_REQUEST request{};
    request.Header.MessageType = LxGnsMessagePortMappingRequest;
    request.Header.MessageSize = sizeof(request);
    request.Af = Port.Family;
    request.Protocol = Port.Protocol;
    request.Port = Port.Port;
    request.Allocate = allocate;
    static_assert(sizeof(request.Address32) == 16);
    static_assert(sizeof(request.Address32) == sizeof(Port.Address.s6_addr32));
    memcpy(request.Address32, Port.Address.s6_addr32, sizeof(request.Address32));

    const auto& response = m_hvSocketChannel->Transaction(request);

    return response.Result;
}

int GnsPortTracker::HandleRequest(const PortAllocation& Port)
{
    // If the port is already allocated, let the call go through and the kernel will
    // decide if bind() should succeed or not
    // Note: Returning 0 will also cause the port's timeout to be updated

    if (m_allocatedPorts.contains(Port))
    {
        GNS_LOG_INFO("Request for a port that's already reserved (family {}, port {}, protocol {})", Port.Family, Port.Port, Port.Protocol);
        return 0;
    }

    // Ask the host for this port otherwise
    const auto error = RequestPort(Port, true);
    GNS_LOG_INFO(
        "Requested the host for port allocation on port (family {}, port {}, protocol {}) - returned {}", Port.Family, Port.Port, Port.Protocol, error);
    return error;
}

std::optional<GnsPortTracker::BindCall> GnsPortTracker::ReadNextRequest()
{
    // Read the call information
    auto request_value = m_request.try_get(c_bpf_poll_timeout);
    if (!request_value.has_value())
    {
        return {};
    }

    auto callInfo = request_value.value();

    // This logic needs to be defensive because the calling process is blocked until
    // CompleteRequest() is called, so if the call information can't be processed because
    // the caller has done something wrong (bad pointer, fd, or protocol), just let it go through
    // and the kernel will fail it

    try
    {
        return GetCallInfo(callInfo.id, callInfo.pid, callInfo.data.arch, callInfo.data.nr, gsl::make_span(callInfo.data.args));
    }
    catch (const std::exception& e)
    {
        GNS_LOG_ERROR("Fetch to read bind() call info with ID {}lu for pid {}, {}", callInfo.id, callInfo.pid, e.what());
        return {{{}, callInfo.id}};
    }
}

std::optional<GnsPortTracker::BindCall> GnsPortTracker::GetCallInfo(
    uint64_t CallId, pid_t Pid, int Arch, int SysCallNumber, const gsl::span<unsigned long long>& Arguments)
{
    auto ParseSocket = [&](int Socket, size_t AddressPtr, size_t AddressLength) -> std::optional<BindCall> {
        if (AddressLength < sizeof(sockaddr))
        {
            return {{{}, CallId}}; // Invalid sockaddr. Let it go through.
        }

        auto networkNamespace = std::filesystem::read_symlink(std::format("/proc/{}/ns/net", Pid)).string();
        if (networkNamespace != m_networkNamespace)
        {
            GNS_LOG_INFO("Skipping bind() call for pid {} in network namespace {}", Pid, networkNamespace.c_str());
            return {{{}, CallId}}; // Different network namespace. Let it go through.
        }

        auto processMemory = m_seccompDispatcher->ReadProcessMemory(CallId, Pid, AddressPtr, AddressLength);
        if (!processMemory.has_value())
        {
            throw RuntimeErrorWithSourceLocation("Failed to read process memory");
        }

        sockaddr& address = *reinterpret_cast<sockaddr*>(processMemory->data());

        if ((address.sa_family != AF_INET && address.sa_family != AF_INET6) ||
            (address.sa_family == AF_INET6 && AddressLength < sizeof(sockaddr_in6)))
        {
            return {{{}, CallId}}; // This is a non IP call, or invalid sockaddr_in6. Let it go through
        }

        // Read the port.  The port *happens* to be in the same spot in memory for both sockaddr_in
        // and sockaddr_in6. To avoid a second memory read, we take advantage of this fact to fetch
        // the port from the currently read memory, regardless of the address family.
        static_assert(sizeof(sockaddr_in) <= sizeof(sockaddr));

        const auto* inAddr = reinterpret_cast<sockaddr_in*>(&address);
        in_port_t port = ntohs(inAddr->sin_port);
        if (port == 0)
        {
            return {{{}, CallId}}; // If port is 0, just let the call go through
        }

        in6_addr storedAddress = {};

        if (address.sa_family == AF_INET)
        {
            storedAddress.s6_addr32[0] = inAddr->sin_addr.s_addr;
        }
        else
        {
            const auto* inAddr6 = reinterpret_cast<sockaddr_in6*>(&address);
            memcpy(storedAddress.s6_addr32, inAddr6->sin6_addr.s6_addr32, sizeof(storedAddress.s6_addr32));
        }

        // It's possible that the calling process lied and passed a sockaddr that
        // doesn't match the underlying socket family or a bad fd. If that's the case,
        // then GetSocketProtocol() will throw
        const int protocol = GetSocketProtocol(Pid, Socket);

        // As GetSocketProtocol interacts with /proc/<pid>, to avoid TOCTOU races we need to
        // verify that call is still valid (call id is the same thing as cookie)
        if (!m_seccompDispatcher->ValidateCookie(CallId))
        {
            throw RuntimeErrorWithSourceLocation(std::format("Invalid call id {}", CallId));
        }

        return {{{PortAllocation(port, address.sa_family, protocol, storedAddress)}, CallId}};
    };
#ifdef __x86_64__
    if (Arch & __AUDIT_ARCH_64BIT)
    {
        return ParseSocket(Arguments[0], Arguments[1], Arguments[2]);
    }
    // Note: 32bit on x86_64 uses the __NR_socketcall with the first argument
    // set to SYS_BIND to make bind system call and the second argument is
    // a pointer to a block of memory containing the original arguments.
    else
    {
        if (Arguments[0] != SYS_BIND)
        {
            return {{{}, CallId}}; // Not a bind call, just let the call go through
        }
        // Grab the first 3 parameters
        auto processMemory = m_seccompDispatcher->ReadProcessMemory(CallId, Pid, Arguments[1], sizeof(uint32_t) * 3);
        if (!processMemory.has_value())
        {
            throw RuntimeErrorWithSourceLocation("Failed to read process memory");
        }

        uint32_t* CopiedArguments = reinterpret_cast<uint32_t*>(processMemory->data());
        return ParseSocket(CopiedArguments[0], CopiedArguments[1], CopiedArguments[2]);
    }
#else
    return ParseSocket(Arguments[0], Arguments[1], Arguments[2]);
#endif
}

void GnsPortTracker::CompleteRequest(uint64_t id, int result)
{
    m_reply.post(result);
}

int GnsPortTracker::GetSocketProtocol(int pid, int fd)
{
    const auto path = std::format("/proc/{}/fd/{}", pid, fd);

    // Because there's a race between the time where the buffer size is determined and
    // and the actual getxattr() call, retry until the buffer size is big enough
    std::string protocol;
    int result = -1;
    do
    {
        int bufferSize = Syscall(getxattr, path.c_str(), "system.sockprotoname", nullptr, 0);
        protocol.resize(std::max(0, bufferSize - 1));

        result = getxattr(path.c_str(), "system.sockprotoname", protocol.data(), bufferSize);
    } while (result < 0 && errno == ERANGE);

    if (result < 0)
    {
        RuntimeErrorWithSourceLocation(std::format("Failed to read protocol for socket: {}, {}", path, errno));
    }

    // In case the size of the attribute shrunk between the two getxattr calls
    protocol.resize(std::max(0, result - 1));

    if (protocol == "TCP" || protocol == "TCPv6")
    {
        return IPPROTO_TCP;
    }
    else if (protocol == "UDP" || protocol == "UDPv6")
    {
        return IPPROTO_UDP;
    }

    throw RuntimeErrorWithSourceLocation(std::format("Unexpected IP socket protocol: {}", protocol));
}

std::ostream& operator<<(std::ostream& out, const GnsPortTracker::PortAllocation& entry)
{
    return out << "Port=" << entry.Port << ", Family=" << entry.Family << ", Protocol=" << entry.Protocol;
}
