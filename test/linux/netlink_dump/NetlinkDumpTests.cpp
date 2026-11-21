// Copyright (C) Microsoft Corporation. All rights reserved.
#include "common.h"
#include "NetlinkChannel.h"
#include "NetlinkTransactionError.h"
#include <linux/inet_diag.h>
#include <linux/sock_diag.h>
#include <iostream>

enum class Expected
{
    Success,
    Error,
    InvalidCompletion,
};

struct Request
{
    int family = NETLINK_SOCK_DIAG;
    int type = SOCK_DIAG_BY_FAMILY;
    int flags = NLM_F_DUMP;
};

template <typename T>
void Append(std::vector<char>& buffer, uint16_t type, uint16_t flags, uint32_t sequence, const T& payload)
{
    const auto offset = buffer.size();
    buffer.resize(offset + NLMSG_SPACE(sizeof(T)));
    auto* header = reinterpret_cast<nlmsghdr*>(buffer.data() + offset);
    header->nlmsg_len = NLMSG_LENGTH(sizeof(T));
    header->nlmsg_type = type;
    header->nlmsg_flags = flags;
    header->nlmsg_seq = sequence;
    memcpy(NLMSG_DATA(header), &payload, sizeof(payload));
}

bool Check(
    const char* name,
    uint16_t dataFlags,
    uint16_t doneFlags,
    int status,
    Expected expected,
    Request parameters = {},
    bool acknowledgement = false,
    size_t donePayloadSize = sizeof(int),
    bool secondData = false)
{
    int descriptors[2];
    THROW_LAST_ERROR_IF(socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors) < 0);
    auto original = NetlinkChannel::FromFd(descriptors[0], parameters.family);
    NetlinkChannel channel(std::move(original));
    const timeval timeout{5, 0};
    for (int descriptor : descriptors)
    {
        THROW_LAST_ERROR_IF(setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0);
    }
    std::thread kernel([&] {
        wil::unique_fd fd{descriptors[1]};
        std::array<char, 4096> request{};
        THROW_LAST_ERROR_IF(recv(fd.get(), request.data(), request.size(), 0) < 0);
        const auto* header = reinterpret_cast<const nlmsghdr*>(request.data());
        std::vector<char> response;
        if (acknowledgement)
        {
            nlmsgerr error{};
            error.error = status;
            Append(response, NLMSG_ERROR, 0, header->nlmsg_seq, error);
        }
        else
        {
            if (parameters.flags & NLM_F_DUMP)
            {
                inet_diag_msg socket{};
                socket.idiag_family = AF_INET;
                Append(response, SOCK_DIAG_BY_FAMILY, secondData ? NLM_F_MULTI : dataFlags, header->nlmsg_seq, socket);
                if (secondData)
                {
                    Append(response, SOCK_DIAG_BY_FAMILY, dataFlags, header->nlmsg_seq, socket);
                }
            }
            const auto offset = response.size();
            response.resize(offset + NLMSG_SPACE(donePayloadSize));
            auto* done = reinterpret_cast<nlmsghdr*>(response.data() + offset);
            done->nlmsg_len = NLMSG_LENGTH(donePayloadSize);
            done->nlmsg_type = NLMSG_DONE;
            done->nlmsg_flags = doneFlags;
            done->nlmsg_seq = header->nlmsg_seq;
            memcpy(NLMSG_DATA(done), &status, donePayloadSize);
        }
        THROW_LAST_ERROR_IF(send(fd.get(), response.data(), response.size(), 0) < 0);
    });

    inet_diag_req_v2 request{};
    request.sdiag_family = AF_INET;
    request.sdiag_protocol = IPPROTO_TCP;
    auto transaction = channel.CreateTransaction(request, parameters.type, parameters.flags);
    bool complete = false;
    bool rejected = false;
    std::optional<int> error;
    try
    {
        transaction.Execute([](const NetlinkResponse&) {});
        complete = true;
    }
    catch (const NetlinkTransactionError& exception)
    {
        rejected = true;
        error = exception.Error();
    }
    kernel.join();
    const bool passed = expected == Expected::Success ? complete
                        : expected == Expected::Error ? rejected && error == (acknowledgement || status != 0 ? status : -EINTR)
                                                      : rejected && !error;
    std::cout << name << ": " << (passed ? "PASS" : "FAIL") << " complete=" << complete << " error=" << error.value_or(0) << '\n';
    return passed;
}

template <typename TRequest, typename TResponse>
bool CheckKernelDump(const char* name, const TRequest& request, int requestType, int responseType, int flags = NLM_F_DUMP, NetlinkChannel channel = {})
{
    auto transaction = channel.CreateTransaction(request, requestType, flags);
    size_t entries = 0;
    try
    {
        transaction.Execute([&](const NetlinkResponse& response) { entries += response.Messages<TResponse>(responseType).size(); });
    }
    catch (const std::exception& exception)
    {
        std::cout << name << ": FAIL " << exception.what() << '\n';
        return false;
    }
    std::cout << name << ": PASS entries=" << entries << '\n';
    return true;
}

int main()
{
    alarm(30);
    bool passed = true;
    passed = Check("interruption on data", NLM_F_MULTI | NLM_F_DUMP_INTR, NLM_F_MULTI, 0, Expected::Error) && passed;
    passed = Check("interruption on final done", NLM_F_MULTI, NLM_F_MULTI | NLM_F_DUMP_INTR, 0, Expected::Error) && passed;
    passed = Check("nonzero done status", NLM_F_MULTI, NLM_F_MULTI, -EINTR, Expected::Error) && passed;
    passed = Check("allocation failure done status", NLM_F_MULTI, NLM_F_MULTI, -ENOMEM, Expected::Error) && passed;
    passed = Check("successful zero-status dump", NLM_F_MULTI, NLM_F_MULTI, 0, Expected::Success) && passed;
    passed = Check("successful acknowledgement", 0, 0, 0, Expected::Success, {.flags = 0}, true) && passed;
    passed = Check("empty custom do completion", 0, 0, 0, Expected::Success, {.flags = 0}, false, 0) && passed;
    passed = Check("missing dump completion status", NLM_F_MULTI, NLM_F_MULTI, 0, Expected::InvalidCompletion, {}, false, 0) && passed;
    passed =
        Check("truncated dump completion status", NLM_F_MULTI, NLM_F_MULTI, 0, Expected::InvalidCompletion, {}, false, sizeof(int) - 1) && passed;
    passed = Check("normal error acknowledgement", 0, 0, -EINVAL, Expected::Error, {.flags = 0}, true) && passed;
    passed =
        Check("interruption on second data message", NLM_F_MULTI | NLM_F_DUMP_INTR, NLM_F_MULTI, 0, Expected::Error, {}, false, sizeof(int), true) &&
        passed;
    passed = Check("custom do completion payload", 0, 0, 123, Expected::Success, {.flags = 0}) && passed;
    for (int flags : {NLM_F_ROOT, NLM_F_MATCH})
    {
        const auto prefix = flags == NLM_F_ROOT ? "ROOT" : "MATCH";
        const Request route{NETLINK_ROUTE, RTM_GETLINK, flags};
        passed = Check(std::format("{} route interrupted data", prefix).c_str(), NLM_F_MULTI | NLM_F_DUMP_INTR, NLM_F_MULTI, 0, Expected::Error, route) &&
                 passed;
        passed = Check(std::format("{} route interrupted done", prefix).c_str(), NLM_F_MULTI, NLM_F_MULTI | NLM_F_DUMP_INTR, 0, Expected::Error, route) &&
                 passed;
        passed = Check(std::format("{} route failed done", prefix).c_str(), NLM_F_MULTI, NLM_F_MULTI, -ENOMEM, Expected::Error, route) && passed;
        passed =
            Check(std::format("{} route missing status", prefix).c_str(), NLM_F_MULTI, NLM_F_MULTI, 0, Expected::InvalidCompletion, route, false, 0) &&
            passed;
        passed =
            Check(std::format("{} route truncated status", prefix).c_str(), NLM_F_MULTI, NLM_F_MULTI, 0, Expected::InvalidCompletion, route, false, sizeof(int) - 1) &&
            passed;
        passed = Check(std::format("{} route success", prefix).c_str(), NLM_F_MULTI, NLM_F_MULTI, 0, Expected::Success, route) && passed;
        passed =
            Check(std::format("{} socket interrupted data", prefix).c_str(), NLM_F_MULTI | NLM_F_DUMP_INTR, NLM_F_MULTI, 0, Expected::Error, {.flags = flags}) &&
            passed;
        passed = Check(std::format("{} socket failed done", prefix).c_str(), NLM_F_MULTI, NLM_F_MULTI, -ENOMEM, Expected::Error, {.flags = flags}) &&
                 passed;
    }
    for (int flags : {NLM_F_REPLACE, NLM_F_EXCL, NLM_F_REPLACE | NLM_F_EXCL})
    {
        const Request create{NETLINK_ROUTE, RTM_NEWLINK, flags};
        passed = Check(std::format("NEW flags {} empty completion", flags).c_str(), 0, 0, 0, Expected::Success, create, false, 0) && passed;
        passed = Check(std::format("NEW flags {} custom completion", flags).c_str(), 0, 0, 123, Expected::Success, create) && passed;
    }
    passed = Check("generic ROOT do completion", 0, 0, 123, Expected::Success, {NETLINK_GENERIC, RTM_GETLINK, NLM_F_ROOT}) && passed;
    passed = Check("generic MATCH do completion", 0, 0, 123, Expected::Success, {NETLINK_GENERIC, RTM_GETLINK, NLM_F_MATCH}) && passed;
    passed = Check("generic dump with NEW-like family ID", NLM_F_MULTI, NLM_F_MULTI, -ENOMEM, Expected::Error, {NETLINK_GENERIC, RTM_NEWLINK, NLM_F_DUMP}) &&
             passed;
    for (int type : {TCPDIAG_GETSOCK, DCCPDIAG_GETSOCK})
    {
        passed = Check(
                     std::format("legacy socket type {} ROOT dump", type).c_str(),
                     NLM_F_MULTI,
                     NLM_F_MULTI,
                     -ENOMEM,
                     Expected::Error,
                     {NETLINK_SOCK_DIAG, type, NLM_F_ROOT}) &&
                 passed;
    }
    passed = Check("socket destroy custom completion", 0, 0, 123, Expected::Success, {NETLINK_SOCK_DIAG, SOCK_DESTROY, NLM_F_DUMP}) && passed;
    passed = Check("route delete custom completion", 0, 0, 123, Expected::Success, {NETLINK_ROUTE, RTM_DELLINK, NLM_F_DUMP}) && passed;
    ifinfomsg link{};
    link.ifi_family = AF_UNSPEC;
    passed = CheckKernelDump<ifinfomsg, ifinfomsg>("real interface dump", link, RTM_GETLINK, RTM_NEWLINK) && passed;
    for (int flags : {NLM_F_ROOT, NLM_F_MATCH})
    {
        passed = CheckKernelDump<ifinfomsg, ifinfomsg>(
                     flags == NLM_F_ROOT ? "real ROOT interface dump" : "real MATCH interface dump", link, RTM_GETLINK, RTM_NEWLINK, flags) &&
                 passed;
    }
    for (int family : {AF_INET, AF_INET6})
    {
        rtmsg route{};
        route.rtm_family = family;
        passed = CheckKernelDump<rtmsg, rtmsg>(family == AF_INET ? "real IPv4 route dump" : "real IPv6 route dump", route, RTM_GETROUTE, RTM_NEWROUTE) &&
                 passed;
    }
    for (int family : {AF_INET, AF_INET6})
    {
        for (int protocol : {IPPROTO_TCP, IPPROTO_UDP})
        {
            wil::unique_fd descriptor{socket(AF_NETLINK, SOCK_RAW, NETLINK_SOCK_DIAG)};
            THROW_LAST_ERROR_IF(!descriptor);
            sockaddr_nl address{};
            address.nl_family = AF_NETLINK;
            THROW_LAST_ERROR_IF(bind(descriptor.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0);
            inet_diag_req_v2 request{};
            request.sdiag_family = family;
            request.sdiag_protocol = protocol;
            request.idiag_states = ~0u;
            passed = CheckKernelDump<inet_diag_req_v2, inet_diag_msg>(
                         std::format("wrapped socket family {} protocol {}", family, protocol).c_str(),
                         request,
                         SOCK_DIAG_BY_FAMILY,
                         SOCK_DIAG_BY_FAMILY,
                         NLM_F_DUMP,
                         NetlinkChannel::FromFd(descriptor.release(), NETLINK_SOCK_DIAG)) &&
                     passed;
        }
    }
    return passed ? 0 : 1;
}
