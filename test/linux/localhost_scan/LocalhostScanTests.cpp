// Copyright (C) Microsoft Corporation. All rights reserved.
#include "../../../src/linux/init/localhost.cpp"
#include <condition_variable>

int g_LogFd = STDERR_FILENO;
int g_TelemetryFd = -1;
std::optional<bool> g_EnableSocketLogging = false;

struct Finished
{
};
struct Query
{
    int family;
    std::vector<int> ports;
    int error = 0;
};
struct Event
{
    bool start;
    int family;
    int port;
};
std::mutex mutex;
std::condition_variable barrierChanged;
std::vector<Event> events;
std::vector<size_t> counts;
int guestFd = -1;
int fakeNetlink = -1;
int monitorFd = -1;
size_t barriers = 0;
size_t scanLimit = 0;
bool ipv4Only = false;
constexpr auto Barrier = LxGnsMessageSetPortListener;

extern "C" int __real_socket(int, int, int);
extern "C" int __real_bind(int, const sockaddr*, socklen_t);
extern "C" int __real_nanosleep(const timespec*, timespec*);
extern "C" int __wrap_socket(int family, int type, int protocol)
{
    if (family == AF_NETLINK && protocol == NETLINK_SOCK_DIAG && fakeNetlink >= 0)
    {
        monitorFd = dup(fakeNetlink);
        return monitorFd;
    }
    if (family == AF_INET6)
    {
        if (fakeNetlink >= 0)
        {
            return __real_socket(AF_UNIX, type, 0);
        }
        if (ipv4Only)
        {
            errno = EAFNOSUPPORT;
            return -1;
        }
    }
    return __real_socket(family, type, protocol);
}
extern "C" int __wrap_bind(int fd, const sockaddr* address, socklen_t length)
{
    return fd == monitorFd && monitorFd >= 0 ? 0 : __real_bind(fd, address, length);
}

extern "C" int __wrap_nanosleep(const timespec* request, timespec* remaining)
{
    if (fakeNetlink < 0)
    {
        return __real_nanosleep(request, remaining);
    }
    MESSAGE_HEADER marker{};
    marker.MessageSize = sizeof(marker);
    marker.MessageType = Barrier;
    marker.TransactionStep = static_cast<uint32_t>(TRANSACTION_STEP::REQUEST);
    THROW_LAST_ERROR_IF(UtilWriteBuffer(guestFd, &marker, sizeof(marker)) < 0);
    std::unique_lock lock(::mutex);
    const auto wanted = counts.size() + 1;
    if (!barrierChanged.wait_for(lock, std::chrono::seconds(5), [&] { return barriers >= wanted; }))
    {
        throw std::runtime_error("Host barrier timed out");
    }
    counts.push_back(events.size());
    if (counts.size() == scanLimit)
    {
        throw Finished{};
    }
    return 0;
}

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

bool Run(const char* name, const std::vector<Query>& queries, const std::vector<size_t>& expected, std::vector<int> hostResults = {})
{
    events.clear();
    counts.clear();
    barriers = 0;
    scanLimit = expected.size();
    int control[2];
    int netlink[2];
    THROW_LAST_ERROR_IF(socketpair(AF_UNIX, SOCK_STREAM, 0, control) < 0);
    THROW_LAST_ERROR_IF(socketpair(AF_UNIX, SOCK_DGRAM, 0, netlink) < 0);
    const timeval receiveTimeout{5, 0};
    THROW_LAST_ERROR_IF(setsockopt(netlink[1], SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout)) < 0);
    guestFd = control[0];
    fakeNetlink = netlink[0];
    std::thread kernel([&] {
        wil::unique_fd fd{netlink[1]};
        for (const auto& query : queries)
        {
            std::array<char, 4096> request{};
            const auto received = recv(fd.get(), request.data(), request.size(), 0);
            if (received < static_cast<ssize_t>(NLMSG_LENGTH(sizeof(inet_diag_req_v2))))
            {
                std::abort();
            }
            const auto* header = reinterpret_cast<const nlmsghdr*>(request.data());
            const auto* payload = reinterpret_cast<const inet_diag_req_v2*>(NLMSG_DATA(header));
            if (payload->sdiag_family != query.family)
            {
                std::abort();
            }
            std::vector<char> response;
            for (int port : query.ports)
            {
                inet_diag_msg socket{};
                socket.idiag_family = query.family;
                socket.id.idiag_sport = htons(port);
                socket.id.idiag_src[query.family == AF_INET ? 0 : 3] = htonl(query.family == AF_INET ? INADDR_LOOPBACK : 1);
                Append(response, SOCK_DIAG_BY_FAMILY, NLM_F_MULTI, header->nlmsg_seq, socket);
            }
            if (!response.empty())
            {
                THROW_LAST_ERROR_IF(send(fd.get(), response.data(), response.size(), 0) < 0);
            }
            response.clear();
            if (query.error)
            {
                nlmsgerr error{};
                error.error = query.error;
                Append(response, NLMSG_ERROR, 0, header->nlmsg_seq, error);
            }
            else
            {
                Append(response, NLMSG_DONE, NLM_F_MULTI, header->nlmsg_seq, int{0});
            }
            THROW_LAST_ERROR_IF(send(fd.get(), response.data(), response.size(), 0) < 0);
        }
    });
    std::thread host([&] {
        wsl::shared::SocketChannel channel(wil::unique_fd{control[1]}, "host");
        size_t startIndex = 0;
        for (;;)
        {
            auto transaction = channel.ReceiveTransaction();
            auto [header, bytes] = transaction.ReceiveOrClosed<MESSAGE_HEADER>();
            if (!header)
            {
                break;
            }
            if (header->MessageType == Barrier)
            {
                std::lock_guard lock(mutex);
                ++barriers;
                barrierChanged.notify_one();
                continue;
            }
            Event event{};
            bool reply = header->MessageType == LxGnsMessagePortMappingRequest;
            if (reply)
            {
                const auto* request = gslhelpers::try_get_struct<LX_GNS_PORT_ALLOCATION_REQUEST>(bytes);
                event = {request->Allocate, request->Af, request->Port};
            }
            else
            {
                const auto* request = gslhelpers::try_get_struct<LX_GNS_PORT_LISTENER_RELAY>(bytes);
                event = {header->MessageType == LxGnsMessagePortListenerRelayStart, request->Family, request->Port};
            }
            {
                std::lock_guard lock(mutex);
                events.push_back(event);
            }
            if (reply)
            {
                const int result = event.start && startIndex < hostResults.size() ? hostResults[startIndex] : 0;
                startIndex += event.start;
                transaction.SendResultMessage<int32_t>(result);
            }
        }
    });
    {
        wsl::shared::SocketChannel channel(wil::unique_fd{control[0]}, "guest");
        try
        {
            MonitorListeningSockets(channel);
        }
        catch (const Finished&)
        {
        }
    }
    kernel.join();
    host.join();
    close(netlink[0]);
    fakeNetlink = -1;
    monitorFd = -1;
    const bool passed = counts == expected;
    std::cout << name << ": " << (passed ? "PASS" : "FAIL") << " observed=";
    for (auto count : counts)
    {
        std::cout << count << ',';
    }
    std::cout << " expected=";
    for (auto count : expected)
    {
        std::cout << count << ',';
    }
    std::cout << '\n';
    return passed;
}

bool CheckIpv4OnlySnapshot()
{
    const bool previous = ipv4Only;
    ipv4Only = true;
    auto restore = wil::scope_exit([&] { ipv4Only = previous; });
    wil::unique_fd listener{socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    THROW_LAST_ERROR_IF(!listener);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    THROW_LAST_ERROR_IF(bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0);
    THROW_LAST_ERROR_IF(listen(listener.get(), 1) < 0);
    socklen_t length = sizeof(address);
    THROW_LAST_ERROR_IF(getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) < 0);
    NetlinkChannel channel(SOCK_RAW, NETLINK_SOCK_DIAG);
    const auto snapshot = QueryListeningSockets(channel);
    const bool passed = snapshot && std::any_of(snapshot->begin(), snapshot->end(), [&](const auto& entry) {
                            return entry.ss_family == AF_INET && reinterpret_cast<const sockaddr_in*>(&entry)->sin_port == address.sin_port;
                        });
    std::cout << "IPv4-only snapshot with unavailable IPv6: " << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}

int main(int argc, char** argv)
{
    ipv4Only = argc > 1 && std::string_view(argv[1]) == "--ipv4-only";
    bool passed = CheckIpv4OnlySnapshot();
    const std::vector<Query> seed{{AF_INET, {18765}}, {AF_INET6, {18766}}};
    for (const auto& failure : std::vector<std::vector<Query>>{
             {{AF_INET, {}, -EBUSY}}, {{AF_INET, {18765}}, {AF_INET6, {}, -EBUSY}}, {{AF_INET, {18765}, -EBUSY}}})
    {
        auto queries = seed;
        queries.insert(queries.end(), failure.begin(), failure.end());
        queries.insert(queries.end(), {{AF_INET, {18765}}, {AF_INET6, {}}, {AF_INET, {}}, {AF_INET6, {}}});
        passed = Run("failed scan preserves confirmed state", queries, {2, 2, 3, 4}) && passed;
    }
    passed = Run("complete empty scan removes all", {{AF_INET, {18765}}, {AF_INET6, {18766}}, {AF_INET, {}}, {AF_INET6, {}}}, {2, 4}) && passed;
    return passed ? 0 : 1;
}
