// Copyright (C) Microsoft Corporation. All rights reserved.

#include <filesystem>
#include <optional>
#include <iostream>
#include <linux/audit.h>
#include <linux/sock_diag.h>
#include <linux/inet_diag.h>
#include <linux/net.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <bpf/libbpf.h>
#include "common.h"
#include "NetlinkTransactionError.h"
#include "GnsPortTracker.h"
#include "lxinitshared.h"
#include "bind_monitor.h"
#include "bind_monitor.skel.h"

constexpr size_t c_bind_timeout_seconds = 60;
constexpr auto c_sock_diag_refresh_delay = std::chrono::milliseconds(500);
constexpr auto c_sock_diag_poll_timeout = std::chrono::milliseconds(10);
constexpr auto c_bpf_poll_timeout = std::chrono::milliseconds(500);

GnsPortTracker::GnsPortTracker(
    std::shared_ptr<wsl::shared::SocketChannel> hvSocketChannel,
    NetlinkChannel&& netlinkChannel,
    std::shared_ptr<SecCompDispatcher> seccompDispatcher,
    std::shared_ptr<std::mutex> channelMutex) :
    m_hvSocketChannel(std::move(hvSocketChannel)),
    m_channelMutex(std::move(channelMutex)),
    m_channel(std::move(netlinkChannel)),
    m_seccompDispatcher(seccompDispatcher)
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

void GnsPortTracker::OnBindEvent(void* Data)
{
    const auto* event = static_cast<const bind_event*>(Data);

    in6_addr address = {};
    if (event->family == AF_INET)
    {
        address.s6_addr32[0] = event->addr4;
    }
    else if (event->family == AF_INET6)
    {
        memcpy(address.s6_addr32, event->addr6, sizeof(address.s6_addr32));
    }
    else
    {
        return;
    }

    PortAllocation allocation(event->port, event->family, event->protocol, address);

    // Check if this port is already tracked by the seccomp path (non-zero bind).
    // If it is, this is not a port-0 bind — skip it.
    {
        std::lock_guard lock(m_portsMutex);
        if (m_allocatedPorts.contains(allocation))
        {
            return;
        }
    }

    // This is a port-0 bind that seccomp let through. Report it to the host.
    const auto result = HandleRequest(allocation);
    if (result == 0)
    {
        TrackPort(std::move(allocation));
    }
    else
    {
        GNS_LOG_ERROR(
            "Failed to register port-0 bind: family ({}) port ({}) protocol ({}), error {}",
            allocation.Family,
            allocation.Port,
            allocation.Protocol,
            result);
    }
}

void GnsPortTracker::Run()
{
    std::thread{std::bind(&GnsPortTracker::RunPortRefresh, this)}.detach();

    // Start eBPF bind monitor on a background thread to handle port-0 bind resolution.
    std::thread{[this]() {
        try
        {
            auto* skel = bind_monitor_bpf__open_and_load();
            if (!skel)
            {
                LOG_ERROR("Failed to open/load bind monitor BPF program, {}", errno);
                return;
            }

            auto destroySkel = wil::scope_exit([&] { bind_monitor_bpf__destroy(skel); });

            if (bind_monitor_bpf__attach(skel) != 0)
            {
                LOG_ERROR("Failed to attach bind monitor BPF program, {}", errno);
                return;
            }

            auto onEvent = [](void* ctx, void* data, size_t dataSz) noexcept -> int {
                try
                {
                    static_cast<GnsPortTracker*>(ctx)->OnBindEvent(data);
                }
                catch (...)
                {
                    LOG_CAUGHT_EXCEPTION_MSG("Error processing bind monitor event");
                }
                return 0;
            };

            auto* rb = ring_buffer__new(bpf_map__fd(skel->maps.events), onEvent, this, nullptr);
            if (!rb)
            {
                LOG_ERROR("Failed to create bind monitor ring buffer, {}", errno);
                return;
            }

            auto destroyRb = wil::scope_exit([&] { ring_buffer__free(rb); });

            GNS_LOG_INFO("BPF bind monitor for port-0 resolution attached and running");

            for (;;)
            {
                int err = ring_buffer__poll(rb, -1);
                if (err == -EINTR)
                {
                    continue;
                }

                if (err < 0)
                {
                    LOG_ERROR("bind monitor ring_buffer__poll failed, {}", err);
                    return;
                }
            }
        }
        CATCH_LOG()
    }}.detach();

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
                    TrackPort(allocationRequest);
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

            // Port-0 binds are now handled by the eBPF callback (OnBindEvent),
            // so no deferred resolution is needed here.
        }

        // If bindCall is empty, then the read() timed out. Look for any closed port
        if (future.has_value() && future->wait_for(c_sock_diag_poll_timeout) == std::future_status::ready)
        {
            refreshResult.emplace(future->get());
            future.reset();
            m_allocatedPortsRefresh = {};

            if (!bindCall.has_value())
            {
                std::lock_guard lock(m_portsMutex);
                OnRefreshAllocatedPorts(refreshResult->Ports, refreshResult->Timestamp);
            }
        }

        // Only look at bound ports if there's something to deallocate to avoid wasting cycles
        if (refreshResult.has_value())
        {
            std::lock_guard lock(m_portsMutex);
            if (!m_allocatedPorts.empty())
            {
                future = m_allocatedPortsRefresh.get_future();
                refreshResult->Resume();
                refreshResult.reset();
            }
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
    // m_portsMutex must be held by caller.
    //
    // Because there's no way to get notified when the bind() call actually completes, it's possible
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

    std::lock_guard lock(*m_channelMutex);
    const auto& response = m_hvSocketChannel->Transaction(request);

    return response.Result;
}

int GnsPortTracker::HandleRequest(const PortAllocation& Port)
{
    {
        std::lock_guard lock(m_portsMutex);
        if (m_allocatedPorts.contains(Port))
        {
            GNS_LOG_INFO("Request for a port that's already reserved (family {}, port {}, protocol {})", Port.Family, Port.Port, Port.Protocol);
            return 0;
        }
    }

    // Ask the host for this port otherwise
    const auto error = RequestPort(Port, true);
    GNS_LOG_INFO(
        "Requested the host for port allocation on port (family {}, port {}, protocol {}) - returned {}", Port.Family, Port.Port, Port.Protocol, error);
    return error;
}

std::optional<GnsPortTracker::BindCall> GnsPortTracker::ReadNextRequest()
{
    auto request_value = m_request.try_get(c_bpf_poll_timeout);
    if (!request_value.has_value())
    {
        return {};
    }

    auto callInfo = request_value.value();

    try
    {
        return GetCallInfo(callInfo.id, callInfo.pid, callInfo.data.arch, callInfo.data.nr, gsl::make_span(callInfo.data.args));
    }
    catch (const std::exception& e)
    {
        GNS_LOG_ERROR("Failed to read bind() call info with ID {} for pid {}, {}", callInfo.id, callInfo.pid, e.what());
        return {{{}, false, callInfo.id}};
    }
}

std::optional<GnsPortTracker::BindCall> GnsPortTracker::GetCallInfo(
    uint64_t CallId, pid_t Pid, int Arch, int SysCallNumber, const gsl::span<unsigned long long>& Arguments)
{
    auto ParseSocket = [&](int Socket, size_t AddressPtr, size_t AddressLength) -> std::optional<BindCall> {
        if (AddressLength < sizeof(sockaddr))
        {
            return {{{}, false, CallId}};
        }

        auto networkNamespace = std::filesystem::read_symlink(std::format("/proc/{}/ns/net", Pid)).string();
        if (networkNamespace != m_networkNamespace)
        {
            GNS_LOG_INFO("Skipping bind() call for pid {} in network namespace {}", Pid, networkNamespace.c_str());
            return {{{}, false, CallId}};
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
            return {{{}, false, CallId}};
        }

        static_assert(sizeof(sockaddr_in) <= sizeof(sockaddr));

        const auto* inAddr = reinterpret_cast<sockaddr_in*>(&address);
        in_port_t port = ntohs(inAddr->sin_port);
        if (port == 0)
        {
            // Port 0 means the kernel will assign an ephemeral port.
            // The eBPF fexit handler (OnBindEvent) will resolve the actual port
            // after bind() completes. Just let the call through.
            return {{{}, true, CallId}};
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

        const int protocol = GetSocketProtocol(Pid, Socket);

        if (!m_seccompDispatcher->ValidateCookie(CallId))
        {
            throw RuntimeErrorWithSourceLocation(std::format("Invalid call id {}", CallId));
        }

        return {{{PortAllocation(port, address.sa_family, protocol, storedAddress)}, false, CallId}};
    };
#ifdef __x86_64__
    if (Arch & __AUDIT_ARCH_64BIT)
    {
        return ParseSocket(Arguments[0], Arguments[1], Arguments[2]);
    }
    else
    {
        if (Arguments[0] != SYS_BIND)
        {
            return {{{}, false, CallId}};
        }
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

    // Because there's a race between the time where the buffer size is determined
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
        throw RuntimeErrorWithSourceLocation(std::format("Failed to read protocol for socket: {}, {}", path, errno));
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

void GnsPortTracker::TrackPort(PortAllocation allocation)
try
{
    // Use insert_or_assign so the deallocation timeout is refreshed if the same
    // port key is already present (emplace would silently keep the old entry).
    std::lock_guard lock(m_portsMutex);
    m_allocatedPorts.insert_or_assign(std::move(allocation), std::make_optional(time(nullptr) + c_bind_timeout_seconds));
}
catch (const std::exception& e)
{
    GNS_LOG_ERROR("Failed to track port allocation, {}", e.what());
}

std::ostream& operator<<(std::ostream& out, const GnsPortTracker::PortAllocation& entry)
{
    return out << "Port=" << entry.Port << ", Family=" << entry.Family << ", Protocol=" << entry.Protocol;
}
