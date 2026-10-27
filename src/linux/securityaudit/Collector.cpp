// Copyright (C) Microsoft Corporation. All rights reserved.

#include "Collector.h"
#include "SecurityEvents.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <optional>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <system_error>
#include <linux/types.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

// The supplied shared C ABI uses C11 assertions even in its C++ header.
#if !defined(__clang__)
#define _Static_assert static_assert
#endif
#include "ebpf_plugin_loader.h"
#include "ebpf_arch_macros.h"
#if !defined(__clang__)
#undef _Static_assert
#endif

static_assert(NLOHMANN_JSON_VERSION_MAJOR == 3 && NLOHMANN_JSON_VERSION_MINOR == 12 && NLOHMANN_JSON_VERSION_PATCH == 0);

namespace wsl::audit {
namespace {

    constexpr size_t c_queueCapacity = 256;
    // Filesystem magic values, without linux/magic.h's conflicting definitions in the supplied ABI header.
    constexpr long c_procfsMagic = 0x9fa0;
    constexpr long c_sysfsMagic = 0x62656572;
    constexpr long c_tracefsMagic = 0x74726163;
    // The supplied loader takes ring capacity in 512-byte units, not bytes.
    constexpr unsigned int c_ringBufferUnits = (8 * 1024 * 1024) / 512;
    constexpr auto c_writeTimeout = std::chrono::seconds(2);
    constexpr auto c_drainTimeout = std::chrono::seconds(5);
    using Clock = std::chrono::steady_clock;

    class CollectorLock
    {
    public:
        CollectorLock()
        {
            m_fd = open("/run/wsl-security-audit.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (m_fd < 0)
            {
                throw std::system_error(errno, std::generic_category(), "Cannot open VM audit collector lock");
            }
            if (flock(m_fd, LOCK_EX | LOCK_NB) < 0)
            {
                const auto error = errno;
                close(m_fd);
                m_fd = -1;
                throw std::system_error(error, std::generic_category(), "Another VM audit collector is still active");
            }
        }

        ~CollectorLock()
        {
            close(m_fd);
        }

        CollectorLock(const CollectorLock&) = delete;
        CollectorLock& operator=(const CollectorLock&) = delete;

    private:
        int m_fd{-1};
    };

    void IsolateMountNamespace()
    {
        if (unshare(CLONE_NEWNS) < 0)
        {
            throw std::system_error(errno, std::generic_category(), "Cannot isolate the collector mount namespace");
        }
        if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0)
        {
            throw std::system_error(errno, std::generic_category(), "Cannot prevent collector mount propagation");
        }
    }

    bool HasFilesystem(const char* path, long magic)
    {
        struct statfs filesystem{};
        if (statfs(path, &filesystem) == 0)
        {
            return filesystem.f_type == magic;
        }
        if (errno != ENOENT)
        {
            throw std::system_error(errno, std::generic_category(), std::string("Cannot inspect collector filesystem: ") + path);
        }
        return false;
    }

    void EnsureKernelFilesystem(const char* path, const char* type, long magic)
    {
        if (HasFilesystem(path, magic))
        {
            return;
        }
        if (mkdir(path, 0755) < 0 && errno != EEXIST)
        {
            throw std::system_error(errno, std::generic_category(), std::string("Cannot create collector mountpoint: ") + path);
        }
        struct stat status{};
        if (lstat(path, &status) < 0)
        {
            throw std::system_error(errno, std::generic_category(), std::string("Cannot inspect collector mountpoint: ") + path);
        }
        if (!S_ISDIR(status.st_mode))
        {
            throw std::runtime_error(std::string("Collector mountpoint is not a directory: ") + path);
        }
        if (mount(type, path, type, MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) < 0)
        {
            throw std::system_error(errno, std::generic_category(), std::string("Cannot mount collector filesystem: ") + path);
        }
    }

    bool PrepareTracefs()
    {
        // Init can move proc/sys into its new root while the launch worker retains the initrd root.
        // Isolate first, even if the mounts still exist, so later init moves cannot remove them here.
        IsolateMountNamespace();
        EnsureKernelFilesystem("/proc", "proc", c_procfsMagic);
        EnsureKernelFilesystem("/sys", "sysfs", c_sysfsMagic);

        for (const auto* path : {"/sys/kernel/tracing", "/sys/kernel/debug/tracing"})
        {
            if (HasFilesystem(path, c_tracefsMagic))
            {
                return false;
            }
        }

        if (mount("tracefs", "/sys/kernel/tracing", "tracefs", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) < 0)
        {
            throw std::system_error(errno, std::generic_category(), "Cannot mount collector tracefs");
        }
        return true;
    }

    bool WriteFrame(int fd, const nlohmann::json& message)
    {
        const auto text = EncodeMessage(message);
        const auto deadline = Clock::now() + c_writeTimeout;
        size_t offset = 0;
        while (offset < text.size() && Clock::now() < deadline)
        {
            pollfd descriptor{fd, POLLOUT, 0};
            const auto result = poll(&descriptor, 1, 100);
            if (result < 0 && errno == EINTR)
            {
                continue;
            }
            if (result < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)))
            {
                return false;
            }
            if (!(descriptor.revents & POLLOUT))
            {
                continue;
            }
            const auto written = write(fd, text.data() + offset, text.size() - offset);
            if (written < 0 && (errno == EINTR || errno == EAGAIN))
            {
                continue;
            }
            if (written <= 0)
            {
                return false;
            }
            offset += static_cast<size_t>(written);
        }
        return offset == text.size();
    }

    class OutputMode
    {
    public:
        explicit OutputMode(int fd) : m_fd(fd), m_flags(fcntl(fd, F_GETFL))
        {
            if (m_flags < 0 || fcntl(fd, F_SETFL, m_flags | O_NONBLOCK) < 0)
            {
                throw std::runtime_error("Cannot configure protocol output");
            }
        }
        ~OutputMode()
        {
            if (fcntl(m_fd, F_SETFL, m_flags) < 0)
            {
                std::fprintf(stderr, "Cannot restore protocol output mode: %s\n", std::strerror(errno));
            }
        }

    private:
        int m_fd;
        int m_flags;
    };

    class InputFrames
    {
    public:
        explicit InputFrames(int fd) : m_fd(fd)
        {
        }

        std::optional<std::string> Next(int timeoutMs)
        {
            auto newline = m_pending.find('\n');
            if (newline != std::string::npos)
            {
                auto line = m_pending.substr(0, newline);
                m_pending.erase(0, newline + 1);
                return line;
            }
            if (m_eof)
            {
                if (!m_pending.empty())
                {
                    throw std::invalid_argument("Unterminated audit input frame");
                }
                return std::nullopt;
            }
            pollfd descriptor{m_fd, POLLIN, 0};
            const auto result = poll(&descriptor, 1, timeoutMs);
            if (result < 0 && errno == EINTR)
            {
                return std::nullopt;
            }
            if (result < 0 || (descriptor.revents & (POLLERR | POLLNVAL)))
            {
                throw std::runtime_error("Audit input poll failed");
            }
            if (result == 0)
            {
                return std::nullopt;
            }
            std::array<char, 4096> buffer{};
            const auto bytes = read(m_fd, buffer.data(), buffer.size());
            if (bytes < 0 && (errno == EINTR || errno == EAGAIN))
            {
                return std::nullopt;
            }
            if (bytes < 0)
            {
                throw std::runtime_error("Audit input read failed");
            }
            if (bytes == 0)
            {
                m_eof = true;
                return Next(0);
            }
            m_pending.append(buffer.data(), static_cast<size_t>(bytes));
            newline = m_pending.find('\n');
            if ((newline == std::string::npos ? m_pending.size() : newline) > c_maxMessageSize)
            {
                throw std::invalid_argument("Audit input exceeds protocol limit");
            }
            if (newline != std::string::npos)
            {
                return Next(0);
            }
            return std::nullopt;
        }

        bool Eof() const
        {
            return m_eof && m_pending.empty();
        }

    private:
        int m_fd;
        bool m_eof{};
        std::string m_pending;
    };

    struct Delivery
    {
        std::mutex lock;
        std::condition_variable available;
        std::deque<std::vector<std::byte>> records;
        bool closing{};
        Clock::time_point drainDeadline{};
        std::atomic<uint64_t> queueDropped{};
        std::atomic<uint64_t> perfLost{};
        std::atomic<uint64_t> invalidRecords{};
        std::atomic<uint64_t> ignoredRecords{};
        std::atomic<uint64_t> policyFiltered{};
        std::atomic<uint64_t> processes{};
        std::atomic<uint64_t> securityEvents{};
        std::atomic<uint64_t> callbackFailures{};
        std::atomic<int> pollError{};
        std::atomic<bool> outputFailed{};
    };

    void EventCallback(void* context, int, void* data, unsigned int size)
    {
        auto& delivery = *static_cast<Delivery*>(context);
        if (!data || size < sizeof(event_begin_s) + sizeof(event_end_s) || size > sizeof(event_exec_process_s))
        {
            ++delivery.invalidRecords;
            return;
        }
        try
        {
            // Dequeue contention is not overflow; decoding and output never hold this lock.
            std::unique_lock lock(delivery.lock);
            if (delivery.records.size() >= c_queueCapacity)
            {
                ++delivery.queueDropped;
                return;
            }
            if (!delivery.closing)
            {
                const auto* bytes = static_cast<const std::byte*>(data);
                delivery.records.emplace_back(bytes, bytes + size);
                delivery.available.notify_one();
            }
        }
        catch (const std::exception&)
        {
            ++delivery.callbackFailures;
        }
    }

    void LostCallback(void* context, int, unsigned long long count)
    {
        static_cast<Delivery*>(context)->perfLost += count;
    }

    void PollErrorCallback(void* context, int error)
    {
        static_cast<Delivery*>(context)->pollError = error == 0 ? -EIO : error;
    }

    void Writer(Delivery& delivery, int outputFd, AuditPolicy policy)
    {
        try
        {
            for (;;)
            {
                std::vector<std::byte> record;
                {
                    std::unique_lock lock(delivery.lock);
                    delivery.available.wait(lock, [&] { return delivery.closing || !delivery.records.empty(); });
                    if (delivery.closing && Clock::now() >= delivery.drainDeadline)
                    {
                        delivery.queueDropped += delivery.records.size();
                        delivery.records.clear();
                        break;
                    }
                    if (delivery.records.empty())
                    {
                        break;
                    }
                    record = std::move(delivery.records.front());
                    delivery.records.pop_front();
                }
                const auto decoded = DecodeRecord(record, policy.processCreation && policy.includeCommandLine);
                if (decoded.status == DecodeStatus::Invalid)
                {
                    ++delivery.invalidRecords;
                    std::fprintf(stderr, "Rejected eBPF record: %s\n", decoded.error.c_str());
                }
                else if (decoded.status == DecodeStatus::Ignored)
                {
                    ++delivery.ignoredRecords;
                }
                else if (
                    decoded.status == DecodeStatus::Process ? !policy.processCreation
                                                            : !AllowsSecurityEvent(
                                                                  policy,
                                                                  decoded.message.at("family").get_ref<const std::string&>(),
                                                                  decoded.message.at("outcome").get_ref<const std::string&>()))
                {
                    ++delivery.policyFiltered;
                }
                else if (!WriteFrame(outputFd, decoded.message))
                {
                    delivery.outputFailed = true;
                    break;
                }
                else
                {
                    if (decoded.status == DecodeStatus::Process)
                    {
                        ++delivery.processes;
                    }
                    else
                    {
                        ++delivery.securityEvents;
                    }
                }
            }
        }
        catch (const std::exception& error)
        {
            delivery.outputFailed = true;
            std::fprintf(stderr, "Audit writer failed: %s\n", error.what());
        }
    }

    class Loader
    {
    public:
        ~Loader()
        {
            if (m_started)
            {
                const auto code = Close();
                if (code != no_error && code != error_not_initialized)
                {
                    std::fprintf(stderr, "eBPF cleanup failed: %d\n", code);
                }
            }
        }

        int Close()
        {
            m_started = false;
            return ebpf_plugin_close();
        }

        void MarkStarted()
        {
            m_started = true;
        }

    private:
        bool m_started{};
    };

    uint32_t PidNamespace()
    {
        struct stat status{};
        if (stat("/proc/self/ns/pid", &status) != 0 || status.st_ino > UINT32_MAX)
        {
            throw std::runtime_error("Cannot read collector PID namespace");
        }
        return static_cast<uint32_t>(status.st_ino);
    }

    std::string DecodePath(const event_kpath_s& path)
    {
        if (path.dentries_number > KPATH_ENTRIES)
        {
            throw std::invalid_argument("Invalid eBPF path component count");
        }
        if (path.dentries_number == 0)
        {
            return {};
        }
        std::string result;
        for (size_t index = path.dentries_number; index > 0; --index)
        {
            const auto* component = path.dentry[index - 1];
            const auto* end = static_cast<const char*>(std::memchr(component, '\0', KPATH_ENTRY_SIZE));
            if (!end)
            {
                throw std::invalid_argument("Unterminated eBPF path component");
            }
            if (end != component)
            {
                if (std::find(component, end, '/') != end)
                {
                    throw std::invalid_argument("Invalid eBPF path component");
                }
                result += '/';
                result.append(component, end);
            }
        }
        return result.empty() ? "/" : result;
    }

    template <typename Record>
    Record ReadRecord(std::span<const std::byte> data)
    {
        if (data.size() != sizeof(Record))
        {
            throw std::invalid_argument("Incompatible security record ABI");
        }
        Record record{};
        std::memcpy(&record, data.data(), sizeof(record));
        return record;
    }

    nlohmann::json OptionalNumber(uint64_t value)
    {
        return value == 0 ? nlohmann::json(nullptr) : nlohmann::json(value);
    }

    nlohmann::json SecurityRecord(const event_base_s& base, std::string_view operation)
    {
        using Json = nlohmann::json;
        const auto credential = [&](uint32_t value) {
            return value == INVALID_CRED || (base.status & STATUS_CRED) != 0 ? Json(nullptr) : Json(value);
        };
        const auto family = SecurityFamily(operation);
        const bool unknownResult = (base.status & STATUS_RC) != 0;
        return {
            {"version", c_protocolVersion},
            {"kind", "security"},
            {"family", family},
            {"operation", operation},
            {"pid", base.pid},
            {"tid", base.tid},
            {"pidNamespace", base.pid_ns_id},
            {"uid", credential(base.uid)},
            {"euid", credential(base.euid)},
            {"gid", credential(base.gid)},
            {"egid", credential(base.egid)},
            {"mountNamespace", OptionalNumber(base.mnt_ns_id)},
            {"networkNamespace", OptionalNumber(base.net_ns_id)},
            {"cgroupId", OptionalNumber(base.cgroup_id)},
            {"timestampNs", OptionalNumber(base.timestamp_exit_ns)},
            {"startTimeNs", OptionalNumber(base.start_time)},
            {"collectionStatus", base.status},
            {"result", unknownResult ? Json(nullptr) : Json(static_cast<int64_t>(base.return_code))},
            {"outcome",
             unknownResult
                 ? "unknown"
                 : (operation == "connect" && base.return_code == -EINPROGRESS ? "pending" : (base.return_code < 0 ? "failure" : "success"))},
            {"fields", EmptySecurityFields(family)}};
    }

    std::string Address(int family, const void* address)
    {
        std::array<char, INET6_ADDRSTRLEN> text{};
        if (!inet_ntop(family, address, text.data(), text.size()))
        {
            throw std::invalid_argument("Invalid security network address");
        }
        return text.data();
    }

    DecodedRecord DecodeSecurityRecord(std::span<const std::byte> data, unsigned int type)
    {
        using Json = nlohmann::json;
        Json message;
        if (type == event_type_fork_process)
        {
            if (data.size() != sizeof(event_fork_process_s) ||
                std::to_integer<unsigned int>(data[offsetof(event_fork_process_s, kthread)]) > 1)
            {
                throw std::invalid_argument("Invalid process-creation record");
            }
            for (const auto offset :
                 {offsetof(event_fork_process_s, kpath_binary) + offsetof(event_kpath_s, is_truncated),
                  offsetof(event_fork_process_s, kpath_cwd) + offsetof(event_kpath_s, is_truncated)})
            {
                if (std::to_integer<unsigned int>(data[offset]) > 1)
                {
                    throw std::invalid_argument("Invalid process-creation path flag");
                }
            }
            const auto record = ReadRecord<event_fork_process_s>(data);
            if (record.kthread)
            {
                return {DecodeStatus::Ignored, {}, {}};
            }
            if (record.base.enriched != 1 || record.base.syscall_id != __NR_fork || record.base.pid == 0 ||
                record.base.pid != record.base.tid)
            {
                throw std::invalid_argument("Process creation requires an enriched new process leader");
            }
            message = SecurityRecord(record.base, "process-created");
            message["result"] = nullptr;
            message["outcome"] = "success";
            auto& fields = message["fields"];
            fields["parentPid"] = record.base.pid2;
            fields["childPid"] = record.base.pid;
            fields["childTid"] = record.base.tid;
            fields["parentStartTimeNs"] = OptionalNumber(record.parent_start_time);
            fields["namespacePid"] = OptionalNumber(record.ns_pid);
            fields["executableTruncated"] = record.kpath_binary.is_truncated;
            fields["cwdTruncated"] = record.kpath_cwd.is_truncated;
            if (record.kpath_binary.dentries_number != 0 && !(record.base.status & STATUS_EXE))
            {
                fields["executable"] = DecodePath(record.kpath_binary);
            }
            if (record.kpath_cwd.dentries_number != 0 && !(record.base.status & STATUS_PWD))
            {
                fields["cwd"] = DecodePath(record.kpath_cwd);
            }
        }
        else if (type == event_type_socket)
        {
            const auto record = ReadRecord<event_socket_s>(data);
            const auto& base = record.base;
            std::string_view operation;
            switch (base.syscall_id)
            {
            case __NR_socket:
                operation = "socket";
                break;
            case __NR_bind:
                operation = "bind";
                break;
            case __NR_connect:
                operation = "connect";
                break;
            case __NR_accept:
                operation = "accept";
                break;
            case __NR_accept4:
                operation = "accept4";
                break;
            default:
                return {DecodeStatus::Ignored, {}, {}};
            }
            message = SecurityRecord(base, operation);
            auto& fields = message["fields"];
            if (operation == "socket")
            {
                if (base.return_code >= 0 && !(base.status & STATUS_RC))
                {
                    fields["socketFd"] = base.return_code;
                }
                if (!(base.status & (STATUS_NOARGS | STATUS_VALUE)))
                {
                    fields["requestedProtocol"] = static_cast<int32_t>(base.a[2]);
                }
            }
            else
            {
                if (!(base.status & STATUS_NOARGS))
                {
                    fields["socketFd"] = static_cast<int32_t>(base.a[0]);
                }
                if ((operation == "accept" || operation == "accept4") && base.return_code >= 0 && !(base.status & STATUS_RC))
                {
                    fields["acceptedFd"] = base.return_code;
                }
            }
            // The pinned musl SDK has no socket identity; those wire fields remain null.
            const auto& endpoint = record.socket_op;
            if (operation != "socket" && !(base.status & (STATUS_VALUE | STATUS_NOARGS)))
            {
                Json ip;
                Json port;
                if (endpoint.addr.sin_family == AF_INET)
                {
                    ip = Address(AF_INET, &endpoint.addr.sin_addr);
                    port = static_cast<uint32_t>(ntohs(endpoint.addr.sin_port));
                }
                else if (endpoint.addr.sin_family == AF_INET6)
                {
                    ip = Address(AF_INET6, &endpoint.addr6.sin6_addr);
                    port = static_cast<uint32_t>(ntohs(endpoint.addr6.sin6_port));
                }
                if (operation == "bind")
                {
                    fields["requestedIp"] = ip;
                    fields["requestedPort"] = port;
                }
                else
                {
                    fields["remoteIp"] = ip;
                    fields["remotePort"] = port;
                }
            }
            if (endpoint.local_port != 0)
            {
                if (endpoint.local_ip_family == AF_INET)
                {
                    fields["localIp"] = Address(AF_INET, &endpoint.local_addr4);
                }
                else if (endpoint.local_ip_family == AF_INET6)
                {
                    fields["localIp"] = Address(AF_INET6, &endpoint.local_addr6);
                }
                if (!fields["localIp"].is_null())
                {
                    fields["localPort"] = endpoint.local_port;
                }
            }
        }
        else if (type == event_type_regular)
        {
            const auto record = ReadRecord<event_regular_s>(data);
            const auto& base = record.base;
            std::string_view operation;
            switch (base.syscall_id)
            {
            case __NR_setuid:
                operation = "setuid";
                break;
            case __NR_setgid:
                operation = "setgid";
                break;
            case __NR_setresgid:
                operation = "setresgid";
                break;
            case __NR_init_module:
                operation = "init_module";
                break;
            case __NR_finit_module:
                operation = "finit_module";
                break;
            default:
                return {DecodeStatus::Ignored, {}, {}};
            }
            message = SecurityRecord(base, operation);
            auto& fields = message["fields"];
            const bool argumentsValid = !(base.status & (STATUS_NOARGS | STATUS_VALUE));
            if (operation == "init_module")
            {
                if (argumentsValid)
                {
                    fields["imageSize"] = base.a[1];
                }
            }
            else if (operation == "finit_module")
            {
                if (argumentsValid)
                {
                    fields["moduleFd"] = static_cast<int32_t>(base.a[0]);
                    fields["flags"] = static_cast<uint32_t>(base.a[2]);
                }
            }
            else
            {
                fields["newUid"] = message["uid"];
                fields["newGid"] = message["gid"];
                if (operation == "setuid")
                {
                    if (argumentsValid)
                    {
                        fields["requestedUid"] = static_cast<uint32_t>(base.a[0]);
                    }
                    fields["oldGid"] = message["gid"];
                }
                else
                {
                    fields["oldUid"] = message["uid"];
                    if (operation == "setgid")
                    {
                        if (argumentsValid)
                        {
                            fields["requestedGid"] = static_cast<uint32_t>(base.a[0]);
                        }
                    }
                    else if (argumentsValid)
                    {
                        fields["requestedRealGid"] = static_cast<uint32_t>(base.a[0]);
                        fields["requestedEffectiveGid"] = static_cast<uint32_t>(base.a[1]);
                        fields["requestedSavedGid"] = static_cast<uint32_t>(base.a[2]);
                    }
                }
            }
        }
        else if (type == event_type_namespace || type == event_type_container_namespace)
        {
            event_base_s base{};
            if (type == event_type_namespace)
            {
                base = ReadRecord<event_namespace_s>(data).base;
            }
            else
            {
                base = ReadRecord<event_container_namespace_s>(data).base;
            }
            if (base.syscall_id != __NR_unshare && base.syscall_id != __NR_setns)
            {
                return {DecodeStatus::Ignored, {}, {}};
            }
            message = SecurityRecord(base, base.syscall_id == __NR_unshare ? "unshare" : "setns");
            auto& fields = message["fields"];
            if (!(base.status & (STATUS_NOARGS | STATUS_VALUE)))
            {
                fields["requestedFlags"] = base.a[base.syscall_id == __NR_unshare ? 0 : 1];
                if (base.syscall_id == __NR_setns)
                {
                    fields["namespaceFd"] = static_cast<int32_t>(base.a[0]);
                }
            }
            if (type == event_type_namespace)
            {
                const auto record = ReadRecord<event_namespace_s>(data);
                fields["namespaceTypes"] = static_cast<uint32_t>(record.namespace_op.types);
                fields["previousMountNamespace"] = OptionalNumber(record.mnt_ns_id);
                fields["operationMountNamespace"] = OptionalNumber(record.namespace_op.mnt.id);
                fields["operationNetworkNamespace"] = OptionalNumber(record.namespace_op.net.id);
                fields["operationPidNamespace"] = OptionalNumber(record.namespace_op.pid.id);
            }
            else
            {
                const auto record = ReadRecord<event_container_namespace_s>(data);
                fields["namespaceTypes"] = static_cast<uint32_t>(record.container_namespace_op.types);
                fields["operationMountNamespace"] = OptionalNumber(record.container_namespace_op.mnt_ns_id);
                fields["operationNetworkNamespace"] = OptionalNumber(record.container_namespace_op.net_ns_id);
                fields["operationPidNamespace"] = OptionalNumber(record.container_namespace_op.pid_ns_id);
            }
        }
        else if (type == event_type_dns_uprobe)
        {
            const auto record = ReadRecord<event_dns_rec>(data);
            const auto& dns = record.dns_op;
            const auto* end = static_cast<const char*>(std::memchr(dns.hostname, '\0', sizeof(dns.hostname)));
            if (!end || dns.ipv4_count > MAX_IPV4_COUNT || dns.ipv6_count > MAX_IPV6_COUNT || dns.status > 1)
            {
                throw std::invalid_argument("Invalid DNS event");
            }
            event_base_s base{};
            base.pid = dns.pid;
            base.tid = dns.tid;
            base.pid_ns_id = dns.pid_ns_id;
            base.timestamp_exit_ns = dns.timestamp;
            base.return_code = dns.return_code;
            base.status = STATUS_CRED;
            message = SecurityRecord(base, "dns-query");
            message["collectionStatus"] = nullptr;
            message["outcome"] = dns.status == 0 ? "success" : "failure";
            auto& fields = message["fields"];
            fields["dnsStatus"] = static_cast<uint32_t>(dns.status);
            fields["dnsName"] = std::string(dns.hostname, end);
            fields["dnsAddresses"] = Json::array();
            for (size_t index = 0; index < dns.ipv4_count; ++index)
            {
                fields["dnsAddresses"].push_back(Address(AF_INET, dns.ipv4_addr[index]));
            }
            for (size_t index = 0; index < dns.ipv6_count; ++index)
            {
                fields["dnsAddresses"].push_back(Address(AF_INET6, dns.ipv6_addr[index]));
            }
        }
        else
        {
            return {DecodeStatus::Ignored, {}, {}};
        }
        ValidateSecurityEvent(message);
        message.dump();
        return {DecodeStatus::Security, std::move(message), {}};
    }

} // namespace

bool MatchesContainerCgroup(std::string_view cgroups, std::string_view containerId)
{
    if (containerId.size() != 64 || !std::all_of(containerId.begin(), containerId.end(), [](char value) {
            return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
        }))
    {
        throw std::invalid_argument("Expected a full Docker container ID");
    }
    const auto scope = "docker-" + std::string(containerId) + ".scope";
    while (!cgroups.empty())
    {
        const auto end = cgroups.find('\n');
        const auto line = cgroups.substr(0, end);
        const auto first = line.find(':');
        const auto second = first == std::string_view::npos ? first : line.find(':', first + 1);
        if (second == std::string_view::npos || second + 1 == line.size() || line[second + 1] != '/')
        {
            throw std::invalid_argument("Invalid proc cgroup membership");
        }
        auto path = line.substr(second + 2);
        while (!path.empty())
        {
            const auto slash = path.find('/');
            const auto component = path.substr(0, slash);
            if (component == containerId || component == scope)
            {
                return true;
            }
            if (slash == std::string_view::npos)
            {
                break;
            }
            path.remove_prefix(slash + 1);
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        cgroups.remove_prefix(end + 1);
    }
    return false;
}

nlohmann::json InspectProcessNamespaces(uint32_t pid, std::string_view containerId)
{
    if (pid == 0 || pid > INT32_MAX)
    {
        throw std::invalid_argument("Namespace inspection requires a positive Linux PID");
    }
    // The inspection helper uses the same native-root launch path, but does not need sysfs or tracing.
    if (!HasFilesystem("/proc", c_procfsMagic))
    {
        IsolateMountNamespace();
        EnsureKernelFilesystem("/proc", "proc", c_procfsMagic);
    }
    const auto path = "/proc/" + std::to_string(pid);
    const auto cleanup = [](DIR* directory) {
        if (closedir(directory) < 0)
        {
            std::fprintf(stderr, "Cannot close namespace inspection directory: %s\n", std::strerror(errno));
        }
    };
    const std::unique_ptr<DIR, decltype(cleanup)> directory(opendir(path.c_str()), cleanup);
    if (!directory)
    {
        throw std::system_error(errno, std::generic_category(), "Cannot open process for namespace inspection");
    }
    timespec observed{};
    if (clock_gettime(CLOCK_MONOTONIC, &observed) < 0)
    {
        throw std::system_error(errno, std::generic_category(), "Cannot read namespace observation clock");
    }
    if (!containerId.empty())
    {
        const auto fileCleanup = [](FILE* file) {
            if (std::fclose(file) != 0)
            {
                std::fprintf(stderr, "Cannot close cgroup inspection file: %s\n", std::strerror(errno));
            }
        };
        const auto cgroupPath = "/proc/self/fd/" + std::to_string(dirfd(directory.get())) + "/cgroup";
        const std::unique_ptr<FILE, decltype(fileCleanup)> file(std::fopen(cgroupPath.c_str(), "re"), fileCleanup);
        if (!file)
        {
            throw std::system_error(errno, std::generic_category(), "Cannot inspect container cgroup membership");
        }
        std::array<char, 8193> buffer{};
        const auto count = std::fread(buffer.data(), 1, buffer.size(), file.get());
        if (std::ferror(file.get()))
        {
            throw std::system_error(errno, std::generic_category(), "Cannot read container cgroup membership");
        }
        if (count == buffer.size() || !MatchesContainerCgroup(std::string_view(buffer.data(), count), containerId))
        {
            throw std::runtime_error("Process no longer belongs to the notified container, or its cgroup layout is unsupported");
        }
    }
    const auto inspect = [&](const char* name) -> uint64_t {
        struct stat status{};
        // Keep the proc directory open so PID reuse cannot redirect the second lookup to another process.
        if (fstatat(dirfd(directory.get()), name, &status, 0) < 0)
        {
            throw std::system_error(errno, std::generic_category(), "Cannot inspect process namespace");
        }
        if (status.st_ino == 0 || status.st_ino > UINT32_MAX)
        {
            throw std::runtime_error("Namespace ID is outside the collector event ABI range");
        }
        return status.st_ino;
    };
    return {
        {"pid", pid},
        {"pidNamespace", inspect("ns/pid")},
        {"mountNamespace", inspect("ns/mnt")},
        {"observedAfterNs", static_cast<uint64_t>(observed.tv_sec) * 1000000000ULL + static_cast<uint64_t>(observed.tv_nsec)}};
}

CollectionPlan BuildCollectionPlan(const AuditPolicy& policy)
{
    CollectionPlan plan;
    if (!policy.AnyEnabled())
    {
        return plan;
    }
    plan.features = PERF_FEATURES_BTF | FEATURE_PID_NS_ID_QUERY | FEATURE_RINGBUFF | FEATURE_ENRICHMENT_AT_SOURCE | FEATURE_RUNTIME_MAP_FLIGHTING;
    plan.runtimeFlags = FEATURE_CONTAINER_RUNTIME_METADATA | FEATURE_SYSCALL_EXTENDED_DATA;
    if (policy.processCreation)
    {
        plan.syscalls.push_back({__NR_execve, true});
        plan.syscalls.push_back({__NR_fork, true});
        plan.runtimeFlags |= FEATURE_SCHED_PROCESS_FORK;
    }
    if (policy.networkSuccess || policy.networkFailure)
    {
        for (const auto syscall : {__NR_socket, __NR_bind, __NR_connect, __NR_accept, __NR_accept4})
        {
            plan.syscalls.push_back({syscall, !policy.networkFailure});
        }
        plan.features |= ENABLE_FEATURE_UPROBE_DNS_ENRICHMENT;
        plan.runtimeFlags |= FEATURE_CONNECT_ENDPOINT_ENRICHMENT | FEATURE_ACCEPT_ENDPOINT_ENRICHMENT;
        if (policy.networkFailure)
        {
            plan.runtimeFlags |= FEATURE_DNS_FAILURE_EVENTS;
        }
    }
    if (policy.moduleLoad)
    {
        plan.syscalls.push_back({__NR_init_module, true});
        plan.syscalls.push_back({__NR_finit_module, true});
    }
    if (policy.privilegeTransition)
    {
        for (const auto syscall : {__NR_setuid, __NR_setgid, __NR_setresgid})
        {
            plan.syscalls.push_back({syscall, true});
        }
    }
    return plan;
}

DecodedRecord DecodeRecord(std::span<const std::byte> data, bool includeCommandLine)
{
    if (data.size() < sizeof(event_begin_s) + sizeof(event_end_s))
    {
        return {DecodeStatus::Invalid, {}, "Short eBPF record"};
    }
    event_begin_s header{};
    event_end_s trailer{};
    std::memcpy(&header, data.data(), sizeof(header));
    std::memcpy(&trailer, data.data() + data.size() - sizeof(trailer), sizeof(trailer));
    if (header.code_bytes_start != CODE_BYTES || trailer.code_bytes_end != CODE_BYTES || header.version != VERSION ||
        header.size != data.size())
    {
        return {DecodeStatus::Invalid, {}, "Invalid eBPF record envelope"};
    }
    if (header.event_type != event_type_exec_process)
    {
        if (header.event_type == event_type_none || header.event_type > _event_type_last)
        {
            return {DecodeStatus::Invalid, {}, "Unknown eBPF event type"};
        }
        try
        {
            return DecodeSecurityRecord(data, header.event_type);
        }
        catch (const std::exception& error)
        {
            return {DecodeStatus::Invalid, {}, error.what()};
        }
    }
    if (data.size() != sizeof(event_exec_process_s))
    {
        return {DecodeStatus::Invalid, {}, "Incompatible exec record ABI"};
    }
    for (const auto offset :
         {offsetof(event_exec_process_s, truncated_cmdline),
          offsetof(event_exec_process_s, kpath_binary) + offsetof(event_kpath_s, is_truncated),
          offsetof(event_exec_process_s, kpath_cwd) + offsetof(event_kpath_s, is_truncated)})
    {
        if (std::to_integer<unsigned int>(data[offset]) > 1)
        {
            return {DecodeStatus::Invalid, {}, "Invalid eBPF boolean"};
        }
    }
    event_exec_process_s event{};
    std::memcpy(&event, data.data(), sizeof(event));
    if (event.base.enriched != 1 || event.base.syscall_id != __NR_execve || event.base.return_code != 0 ||
        event.argsize >= MAX_CMDLINE_ARRAY_SIZE || event.argcount < 0 || event.argcount > MAX_CMDLINE_ARGS)
    {
        return {DecodeStatus::Invalid, {}, "Invalid or non-enriched exec record"};
    }
    try
    {
        nlohmann::json message = {
            {"version", c_protocolVersion},
            {"kind", "process"},
            {"pid", event.base.pid},
            {"tid", event.base.tid},
            {"ppid", event.parent_pid},
            {"pidNamespace", event.base.pid_ns_id},
            {"namespacePid", event.ns_pid},
            {"mountNamespace", event.base.mnt_ns_id},
            {"networkNamespace", event.base.net_ns_id},
            {"cgroupId", event.base.cgroup_id},
            {"pidNamespaceMissing", event.base.pid_ns_id == 0},
            {"collectionStatus", event.base.status},
            {"uid", event.base.uid},
            {"euid", event.base.euid},
            {"gid", event.base.gid},
            {"egid", event.base.egid},
            {"timestampNs", event.operation_time},
            {"startTimeNs", event.base.start_time},
            {"executable", DecodePath(event.kpath_binary)},
            {"cwd", DecodePath(event.kpath_cwd)},
            {"executableMissing", event.kpath_binary.dentries_number == 0 || (event.base.status & STATUS_EXE) != 0},
            {"cwdMissing", event.kpath_cwd.dentries_number == 0 || (event.base.status & STATUS_PWD) != 0},
            {"executableTruncated", event.kpath_binary.is_truncated},
            {"cwdTruncated", event.kpath_cwd.is_truncated},
            {"credentialsMissing",
             event.base.uid == INVALID_CRED || event.base.euid == INVALID_CRED || event.base.gid == INVALID_CRED ||
                 event.base.egid == INVALID_CRED || (event.base.status & STATUS_CRED) != 0},
            {"uidMissing", event.base.uid == INVALID_CRED || (event.base.status & STATUS_CRED) != 0},
            {"euidMissing", event.base.euid == INVALID_CRED || (event.base.status & STATUS_CRED) != 0},
            {"gidMissing", event.base.gid == INVALID_CRED || (event.base.status & STATUS_CRED) != 0},
            {"egidMissing", event.base.egid == INVALID_CRED || (event.base.status & STATUS_CRED) != 0},
            {"startTimeMissing", event.base.start_time == 0},
            {"timestampMissing", event.operation_time == 0},
            {"cmdlineMissing", event.argsize == 0},
            {"cmdlineTruncated", event.truncated_cmdline}};
        size_t offset = 0;
        auto arguments = nlohmann::json::array();
        while (offset < event.argsize)
        {
            const auto* begin = event.cmdline + offset;
            const auto remaining = event.argsize - offset;
            const auto* end = static_cast<const char*>(std::memchr(begin, '\0', remaining));
            if (!end)
            {
                if (!event.truncated_cmdline)
                {
                    return {DecodeStatus::Invalid, {}, "Unterminated untruncated exec argument"};
                }
                // An incomplete final argument consumes no extra byte beyond the kernel's limit.
                if (includeCommandLine && remaining < MAX_CMDLINE_ARRAY_SIZE)
                {
                    arguments.emplace_back(std::string(begin, remaining));
                }
                break;
            }
            if (includeCommandLine)
            {
                arguments.emplace_back(std::string(begin, end));
            }
            offset += static_cast<size_t>(end - begin) + 1;
        }
        if (includeCommandLine)
        {
            message["argv"] = std::move(arguments);
        }
        ValidateProcessEvent(message);
        // Strict UTF-8 validation here prevents malformed kernel strings reaching the writer.
        (void)EncodeMessage(message);
        return {DecodeStatus::Process, std::move(message), {}};
    }
    catch (const std::exception& error)
    {
        return {DecodeStatus::Invalid, {}, std::string("Invalid exec payload: ") + error.what()};
    }
}

bool PrepareCollectorTracefs()
{
    return PrepareTracefs();
}

int ReportCollectorFailure(std::string_view message, int outputFd)
{
    std::fprintf(stderr, "Audit collector: %.*s\n", static_cast<int>(message.size()), message.data());
    WriteFrame(outputFd, {{"version", c_protocolVersion}, {"kind", "error"}, {"message", message}});
    WriteFrame(outputFd, {{"version", c_protocolVersion}, {"kind", "stopped"}, {"exitCode", 1u}, {"reason", "error"}});
    return 1;
}

int RunCollector(const CollectorOptions& options, int inputFd, int outputFd)
{
    signal(SIGPIPE, SIG_IGN);
    OutputMode outputMode(outputFd);
    InputFrames input(inputFd);
    Delivery delivery;
    std::unique_ptr<CollectorLock> collectorLock;
    std::unique_ptr<Loader> loader;
    std::thread writer;
    std::string failure;
    std::string reason{"stop"};
    std::array<uint64_t, drop_reason__count> kernelDrops{};
    try
    {
        if (options.package.empty() || options.duration.count() < 0 || options.duration > std::chrono::hours(24))
        {
            throw std::invalid_argument("Invalid collector package or duration");
        }
        std::optional<std::string> configuration;
        const auto configurationDeadline = Clock::now() + std::chrono::seconds(30);
        while (!configuration && !input.Eof() && Clock::now() < configurationDeadline)
        {
            configuration = input.Next(100);
        }
        if (!configuration)
        {
            throw std::invalid_argument("Missing initial audit configuration");
        }
        const auto policy = ParseConfiguration(*configuration);
        std::string loadedVariant;
        int hooksAttachCode = no_error;
        int initializeCode = no_error;
        bool tracefsPrivateMount = false;
        bool initializationDiagnostics = false;
        std::array<char, 8192> errorBuffer{};
        if (policy.AnyEnabled())
        {
            collectorLock = std::make_unique<CollectorLock>();
            tracefsPrivateMount = PrepareCollectorTracefs();
            if (access("/sys/kernel/btf/vmlinux", R_OK) < 0)
            {
                throw std::system_error(
                    errno, std::generic_category(), "Required kernel BTF is unavailable: /sys/kernel/btf/vmlinux");
            }
            std::fprintf(
                stderr,
                "WSL audit: init static loader, event ABI %d; requires BTF, ring buffer, enrichment, fmap and "
                "CAP_BPF/CAP_PERFMON (or CAP_SYS_ADMIN); tracefs setup may require CAP_SYS_ADMIN.\n",
                VERSION);
            if (policy.networkSuccess || policy.networkFailure)
            {
                std::fprintf(
                    stderr,
                    "WSL audit: DNS hooks only cover libc found in the collector root; "
                    "distribution/container DNS coverage is not established.\n");
            }
            loader = std::make_unique<Loader>();
            const auto plan = BuildCollectionPlan(policy);
            std::vector<syscall_config> configuration;
            for (const auto& syscall : plan.syscalls)
            {
                configuration.push_back({syscall.number, syscall.successOnly});
            }
            loader->MarkStarted();
            const auto info = ebpf_plugin_start(
                options.package.c_str(),
                plan.features,
                plan.runtimeFlags,
                configuration.data(),
                static_cast<int>(configuration.size()),
                nullptr,
                0,
                c_ringBufferUnits,
                nullptr,
                0,
                {},
                errorBuffer.data(),
                static_cast<int>(errorBuffer.size()));
            initializeCode = info.return_code;
            hooksAttachCode = info.hooks_attach_code;
            initializationDiagnostics = errorBuffer[0] != '\0';
            if (initializationDiagnostics)
            {
                std::fprintf(stderr, "WSL audit loader diagnostics: %.*s\n", static_cast<int>(errorBuffer.size()), errorBuffer.data());
            }
            const auto* variantEnd = static_cast<const char*>(std::memchr(info.fileloaded, '\0', sizeof(info.fileloaded)));
            if (!variantEnd)
            {
                throw std::runtime_error("Unterminated loaded eBPF variant");
            }
            loadedVariant.assign(info.fileloaded, variantEnd);
            if (initializeCode < no_error || initializeCode >= error_lowest ||
                (initializeCode != no_error && initializeCode < warning_lowest) || hooksAttachCode != no_error)
            {
                throw std::runtime_error(
                    "eBPF initialization/attachment failed (return=" + std::to_string(initializeCode) +
                    ", hooks=" + std::to_string(hooksAttachCode) + "): " + errorBuffer.data());
            }
            if (!loadedVariant.ends_with("_btf_rbuff_enriched_fmap.o"))
            {
                throw std::runtime_error(
                    "Required BTF/ring/enrichment/fmap variant was not loaded: " + loadedVariant + "; diagnostics: " + errorBuffer.data());
            }
            if (initializeCode != no_error)
            {
                std::fprintf(stderr, "eBPF initialization warning %d: %s\n", initializeCode, errorBuffer.data());
            }
            if (policy.networkSuccess || policy.networkFailure)
            {
                std::fprintf(
                    stderr,
                    "WSL audit: DNS coverage is not verified for this integration; "
                    "kernel network syscall collection remains enabled.\n");
            }
            const auto code = ebpf_plugin_start_polling(
                &delivery, EventCallback, LostCallback, PollErrorCallback, errorBuffer.data(), static_cast<int>(errorBuffer.size()), 64, 8);
            if (code != no_error)
            {
                throw std::runtime_error("eBPF polling startup failed (" + std::to_string(code) + "): " + errorBuffer.data());
            }
        }
        nlohmann::json ready = {
            {"version", c_protocolVersion},
            {"kind", "ready"},
            {"status", policy.AnyEnabled() ? "running" : "disabled"},
            {"collectionEnabled", policy.AnyEnabled()},
            {"includeCommandLine", policy.processCreation && policy.includeCommandLine},
            {"policy", PolicySettings(policy)},
            {"policySource", policy.source},
            {"pidNamespace", PidNamespace()},
            {"loadedVariant", loadedVariant},
            {"hooksAttachCode", hooksAttachCode},
            {"initializeCode", initializeCode},
            {"tracefsPrivateMount", tracefsPrivateMount},
            {"eventAbiVersion", VERSION},
            {"staticLoader", true},
            {"initializationDiagnostics", initializationDiagnostics},
            {"coverage", LibraryCoverage()}};
        if (policy.networkSuccess || policy.networkFailure)
        {
            ready["dnsProbeCount"] = nullptr;
            ready["dnsCoverage"] = "unverified";
            auto& supported = ready["coverage"]["network"]["supported"];
            for (auto operation = supported.begin(); operation != supported.end();)
            {
                operation = *operation == "dns-query" ? supported.erase(operation) : std::next(operation);
            }
        }
        if (!WriteFrame(outputFd, ready))
        {
            throw std::runtime_error("Protocol ready output failed");
        }
        if (policy.AnyEnabled())
        {
            std::fprintf(
                stderr, "WSL audit: partial library coverage; inspect READY coverage for unsupported operations and fields.\n");
        }
        writer = std::thread(Writer, std::ref(delivery), outputFd, policy);
        const auto deadline = options.duration.count() == 0 ? Clock::time_point::max() : Clock::now() + options.duration;
        auto nextHealth = Clock::now() + std::chrono::seconds(1);
        for (;;)
        {
            if (delivery.pollError != 0)
            {
                throw std::runtime_error("eBPF poll callback failure: " + std::to_string(delivery.pollError.load()));
            }
            if (delivery.outputFailed)
            {
                throw std::runtime_error("Audit protocol writer failed");
            }
            if (delivery.invalidRecords != 0 || delivery.callbackFailures != 0)
            {
                throw std::runtime_error("Invalid eBPF records or event callback failure");
            }
            if (Clock::now() >= deadline)
            {
                reason = "duration";
                break;
            }
            const auto frame = input.Next(100);
            if (frame)
            {
                const auto message = ParseMessage(*frame);
                if (message["kind"] != "stop")
                {
                    throw std::invalid_argument("Expected audit stop frame");
                }
                break;
            }
            if (input.Eof())
            {
                reason = "eof";
                break;
            }
            if (loader && Clock::now() >= nextHealth)
            {
                ebpf_prog_drop_counters counters{};
                const auto code =
                    ebpf_plugin_get_events_dropped_buffer_count(&counters, errorBuffer.data(), static_cast<int>(errorBuffer.size()));
                if (code != no_error)
                {
                    throw std::runtime_error("Cannot read ring-buffer drop counters: " + std::string(errorBuffer.data()));
                }
                for (size_t index = 0; index < kernelDrops.size(); ++index)
                {
                    kernelDrops[index] += counters.drop_reasons[index];
                }
                nextHealth = Clock::now() + std::chrono::seconds(1);
            }
        }
    }
    catch (const std::exception& error)
    {
        failure = error.what();
        reason = "error";
    }
    // Only this lifecycle owner closes the library, which joins its polling thread.
    if (loader)
    {
        ebpf_prog_drop_counters counters{};
        std::array<char, 8192> errorBuffer{};
        const auto dropCode =
            ebpf_plugin_get_events_dropped_buffer_count(&counters, errorBuffer.data(), static_cast<int>(errorBuffer.size()));
        if (dropCode == no_error)
        {
            for (size_t index = 0; index < kernelDrops.size(); ++index)
            {
                kernelDrops[index] += counters.drop_reasons[index];
            }
        }
        else if (failure.empty())
        {
            failure = "Final ring-buffer drop counter read failed: " + std::string(errorBuffer.data());
        }
        const auto closeCode = loader->Close();
        if (closeCode != no_error && closeCode != error_not_initialized && failure.empty())
        {
            failure = "eBPF close failed: " + std::to_string(closeCode);
        }
    }
    {
        std::lock_guard lock(delivery.lock);
        delivery.closing = true;
        delivery.drainDeadline = Clock::now() + c_drainTimeout;
        delivery.available.notify_one();
    }
    if (writer.joinable())
    {
        writer.join();
    }
    if (failure.empty() &&
        (delivery.queueDropped != 0 || delivery.perfLost != 0 || delivery.invalidRecords != 0 || delivery.callbackFailures != 0 ||
         delivery.outputFailed || delivery.pollError != 0 || kernelDrops[drop_reason_send_fail] != 0 ||
         kernelDrops[drop_reason_args_map_lookup_fail] != 0 || kernelDrops[drop_reason_event_storage_unavail] != 0))
    {
        failure = "Audit collection lost events or encountered a delivery error; see stopped counters";
    }
    const auto exitCode = failure.empty() ? 0u : 1u;
    if (!failure.empty())
    {
        std::fprintf(stderr, "Audit collector: %s\n", failure.c_str());
        std::fprintf(
            stderr,
            "Audit loss counters: queue=%llu perf=%llu invalid=%llu callback=%llu kernelSend=%llu kernelArgs=%llu "
            "kernelStorage=%llu\n",
            static_cast<unsigned long long>(delivery.queueDropped.load()),
            static_cast<unsigned long long>(delivery.perfLost.load()),
            static_cast<unsigned long long>(delivery.invalidRecords.load()),
            static_cast<unsigned long long>(delivery.callbackFailures.load()),
            static_cast<unsigned long long>(kernelDrops[drop_reason_send_fail]),
            static_cast<unsigned long long>(kernelDrops[drop_reason_args_map_lookup_fail]),
            static_cast<unsigned long long>(kernelDrops[drop_reason_event_storage_unavail]));
        WriteFrame(outputFd, {{"version", c_protocolVersion}, {"kind", "error"}, {"message", failure}});
    }
    const auto stoppedWritten = WriteFrame(
        outputFd,
        {{"version", c_protocolVersion},
         {"kind", "stopped"},
         {"exitCode", exitCode},
         {"reason", failure.empty() ? reason : "error"},
         {"processEvents", delivery.processes.load()},
         {"securityEvents", delivery.securityEvents.load()},
         {"queueDropped", delivery.queueDropped.load()},
         {"perfLost", delivery.perfLost.load()},
         {"invalidRecords", delivery.invalidRecords.load()},
         {"ignoredRecords", delivery.ignoredRecords.load()},
         {"policyFiltered", delivery.policyFiltered.load()},
         {"callbackFailures", delivery.callbackFailures.load()},
         {"pollError", delivery.pollError.load()},
         {"outputFailed", delivery.outputFailed.load()},
         {"kernelDropReasons", kernelDrops}});
    return stoppedWritten ? static_cast<int>(exitCode) : 1;
}

} // namespace wsl::audit
