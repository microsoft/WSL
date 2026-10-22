// Copyright (C) Microsoft Corporation. All rights reserved.

#include "Collector.h"
#include "SecurityEvents.h"
#include "ProcessExecuted.h"
#include "ProcessCreated.h"
#include "ObservationSchema.h"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <linux/types.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <signal.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <thread>
#include <vector>

#if !defined(__clang__)
#define _Static_assert static_assert
#endif
#include "event_defs.h"
#include "ebpf_arch_macros.h"
#if !defined(__clang__)
#undef _Static_assert
#endif

namespace {

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

event_exec_process_s Fixture()
{
    event_exec_process_s event{};
    event.begin = {CODE_BYTES, VERSION, event_type_exec_process, sizeof(event)};
    event.end.code_bytes_end = CODE_BYTES;
    event.base.enriched = 1;
    event.base.syscall_id = __NR_execve;
    event.base.pid = 123;
    event.base.tid = 124;
    event.parent_pid = 100;
    event.base.pid_ns_id = 4026531836u;
    event.ns_pid = 23U;
    event.base.mnt_ns_id = 44U;
    event.base.net_ns_id = 45U;
    event.base.cgroup_id = UINT64_MAX;
    event.operation_time = 1234567890123ULL;
    event.base.start_time = 1230000000000ULL;
    event.kpath_binary.dentries_number = 3;
    std::strcpy(event.kpath_binary.dentry[0], "echo");
    std::strcpy(event.kpath_binary.dentry[1], "bin");
    event.kpath_cwd.dentries_number = 1;
    constexpr char arguments[] = "echo\0\0marker";
    event.argsize = sizeof(arguments);
    std::memcpy(event.cmdline, arguments, sizeof(arguments));
    return event;
}

wsl::audit::DecodedRecord Decode(const event_exec_process_s& event, bool includeCommandLine = true)
{
    return wsl::audit::DecodeRecord({reinterpret_cast<const std::byte*>(&event), sizeof(event)}, includeCommandLine);
}

void NamespaceInspectionTests()
{
    const auto pid = static_cast<uint32_t>(getpid());
    const auto result = wsl::audit::InspectProcessNamespaces(pid);
    struct stat pidStatus{}, mountStatus{};
    Check(stat("/proc/self/ns/pid", &pidStatus) == 0 && stat("/proc/self/ns/mnt", &mountStatus) == 0, "Inspect test namespaces");
    Check(
        result.size() == 4 && result["pid"] == pid && result["pidNamespace"] == pidStatus.st_ino &&
            result["mountNamespace"] == mountStatus.st_ino && result["observedAfterNs"].is_number_unsigned() && result["observedAfterNs"] > 0,
        "Namespace helper output mismatch");
    for (const auto invalid : {0U, UINT32_MAX, static_cast<uint32_t>(INT32_MAX)})
    {
        bool failed{};
        try
        {
            wsl::audit::InspectProcessNamespaces(invalid);
        }
        catch (const std::exception&)
        {
            failed = true;
        }
        Check(failed, "Invalid/nonexistent PID accepted");
    }
    const std::string id(64, 'a');
    Check(wsl::audit::MatchesContainerCgroup("0::/docker/" + id + "\n", id), "cgroupfs container not matched");
    Check(
        wsl::audit::MatchesContainerCgroup("0::/system.slice/docker-" + id + ".scope/child\n", id),
        "systemd container not matched");
    Check(!wsl::audit::MatchesContainerCgroup("0::/docker/prefix" + id + "\n", id), "Container ID substring accepted");
    Check(!wsl::audit::MatchesContainerCgroup("0::/\n", id), "Root cgroup attributed to container");
    bool failed{};
    try
    {
        wsl::audit::InspectProcessNamespaces(pid, id);
    }
    catch (const std::exception&)
    {
        failed = true;
    }
    Check(failed, "Unrelated/reused PID attributed to notified container");
}

void DecoderTests()
{
    auto fixture = Fixture();
    auto decoded = Decode(fixture);
    Check(decoded.status == wsl::audit::DecodeStatus::Process, "Valid enriched exec rejected");
    Check(decoded.message["argv"] == nlohmann::json::array({"echo", "", "marker"}), "Empty argument lost");
    Check(decoded.message["executable"] == "/bin/echo" && decoded.message["cwd"] == "/", "Path decoding");
    Check(decoded.message["timestampNs"] == fixture.operation_time, "Monotonic timestamp altered");
    Check(decoded.message["startTimeNs"] == fixture.base.start_time, "Start time altered");
    wsl::audit::ValidateProcessEvent(decoded.message);
    Check(!Decode(fixture, false).message.contains("argv"), "Command-line redaction failed");
    auto sample = wsl::audit::ProcessExecutedData(decoded.message, true);
    Check(sample["uid"]["status"] == "present" && sample["uid"]["value"] == 0U, "Real root UID lost");
    Check(sample["argv"]["value"][1] == "", "Real empty argument lost");
    Check(
        sample["namespacePid"]["value"] == fixture.ns_pid && sample["mountNamespace"]["value"] == fixture.base.mnt_ns_id &&
            sample["networkNamespace"]["value"] == fixture.base.net_ns_id && sample["cgroupId"]["value"] == "18446744073709551615",
        "Library-supported PM context was not transported losslessly");
    Check(!sample.contains("linuxStartNs"), "Internal start clock leaked into the PM log contract");
    Check(wsl::audit::DecodeRecord({}, true).status == wsl::audit::DecodeStatus::Invalid, "Short header accepted");

    const auto reject = [](event_exec_process_s invalid) {
        Check(Decode(invalid).status == wsl::audit::DecodeStatus::Invalid, "Malformed fixture accepted");
    };
    auto invalid = fixture;
    invalid.begin.code_bytes_start = 0;
    reject(invalid);
    invalid = fixture;
    invalid.end.code_bytes_end = 0;
    reject(invalid);
    invalid = fixture;
    ++invalid.begin.size;
    reject(invalid);
    invalid = fixture;
    ++invalid.begin.version;
    reject(invalid);
    invalid = fixture;
    invalid.begin.event_type = UINT32_MAX;
    reject(invalid);
    invalid = fixture;
    invalid.argsize = MAX_CMDLINE_ARRAY_SIZE + 1;
    reject(invalid);
    invalid = fixture;
    invalid.argcount = MAX_CMDLINE_ARGS + 1;
    reject(invalid);
    invalid = fixture;
    invalid.argcount = -1;
    reject(invalid);
    invalid = fixture;
    invalid.base.enriched = 0;
    reject(invalid);
    invalid = fixture;
    invalid.base.return_code = -1;
    reject(invalid);
    invalid = fixture;
    invalid.base.syscall_id = __NR_getpid;
    reject(invalid);
    invalid = fixture;
    invalid.kpath_binary.dentries_number = KPATH_ENTRIES + 1;
    reject(invalid);
    invalid = fixture;
    std::memset(invalid.kpath_binary.dentry[0], 'x', KPATH_ENTRY_SIZE);
    reject(invalid);
    invalid = fixture;
    std::strcpy(invalid.kpath_binary.dentry[0], "bad/path");
    reject(invalid);
    invalid = fixture;
    invalid.cmdline[0] = static_cast<char>(0xff);
    reject(invalid);
    Check(
        Decode(invalid, false).status == wsl::audit::DecodeStatus::Process,
        "Redacted invalid UTF-8 command line should not cross the boundary");

    std::vector<std::byte> bytes(sizeof(fixture) + 1);
    std::memcpy(bytes.data() + 1, &fixture, sizeof(fixture));
    Check(
        wsl::audit::DecodeRecord({bytes.data() + 1, sizeof(fixture)}, true).status == wsl::audit::DecodeStatus::Process,
        "Unaligned callback data rejected");
    bytes[1 + offsetof(event_exec_process_s, truncated_cmdline)] = std::byte{2};
    Check(
        wsl::audit::DecodeRecord({bytes.data() + 1, sizeof(fixture)}, true).status == wsl::audit::DecodeStatus::Invalid,
        "Invalid bool accepted");

    event_regular_s ignored{};
    ignored.begin = {CODE_BYTES, VERSION, event_type_regular, sizeof(ignored)};
    ignored.end.code_bytes_end = CODE_BYTES;
    Check(
        wsl::audit::DecodeRecord({reinterpret_cast<const std::byte*>(&ignored), sizeof(ignored)}, true).status == wsl::audit::DecodeStatus::Ignored,
        "Non-exec record not ignored");
    ignored.begin.event_type = event_type_exec_process;
    Check(
        wsl::audit::DecodeRecord({reinterpret_cast<const std::byte*>(&ignored), sizeof(ignored)}, true).status == wsl::audit::DecodeStatus::Invalid,
        "Legacy-sized exec accepted");

    fixture.argsize = 0;
    fixture.truncated_cmdline = true;
    decoded = Decode(fixture);
    Check(decoded.message["argv"].empty() && decoded.message["cmdlineMissing"] == true, "Missing argv not preserved");
    fixture.argsize = 3;
    std::memcpy(fixture.cmdline, "abc", 3);
    decoded = Decode(fixture);
    Check(
        decoded.message["argv"] == nlohmann::json::array({"abc"}) && decoded.message["cmdlineTruncated"] == true,
        "Partial argument lost");
    fixture.truncated_cmdline = false;
    reject(fixture);
    fixture.truncated_cmdline = true;
    fixture.argsize = MAX_CMDLINE_ARRAY_SIZE - 1;
    std::memset(fixture.cmdline, 'x', fixture.argsize);
    decoded = Decode(fixture);
    Check(
        decoded.status == wsl::audit::DecodeStatus::Process && decoded.message["argv"][0].get<std::string>().size() == MAX_CMDLINE_ARRAY_SIZE - 1,
        "Maximum truncated argument rejected");
    fixture = Fixture();
    fixture.kpath_binary.dentries_number = 0;
    fixture.kpath_cwd.is_truncated = true;
    fixture.base.uid = INVALID_CRED;
    fixture.base.start_time = 0;
    decoded = Decode(fixture);
    Check(
        decoded.message["executable"] == "" && decoded.message["executableMissing"] == true && decoded.message["cwdTruncated"] == true &&
            decoded.message["credentialsMissing"] == true && decoded.message["startTimeMissing"] == true,
        "Missing enrichment state lost");
    sample = wsl::audit::ProcessExecutedData(decoded.message, true);
    Check(
        sample["uid"]["value"].is_null() && sample["euid"]["value"] == 0U && sample["executable"]["status"] == "unavailable" &&
            sample["cwd"]["truncated"] == true,
        "Collector metadata did not reach sample schema");
    fixture = Fixture();
    fixture.base.status = STATUS_CRED | STATUS_EXE | STATUS_PWD;
    decoded = Decode(fixture);
    sample = wsl::audit::ProcessExecutedData(decoded.message, false);
    Check(
        sample["uid"]["value"].is_null() && sample["euid"]["value"].is_null() && sample["executable"]["value"].is_null() &&
            sample["cwd"]["value"].is_null(),
        "Failed collection flags were treated as valid values");
}

template <typename Record>
Record SecurityRecord(unsigned int type, unsigned long syscall)
{
    Record record{};
    record.begin = {CODE_BYTES, VERSION, type, sizeof(record)};
    record.end.code_bytes_end = CODE_BYTES;
    record.base.syscall_id = syscall;
    record.base.pid = 123;
    record.base.tid = 124;
    record.base.pid_ns_id = 5678;
    record.base.uid = 63001;
    record.base.gid = 62001;
    record.base.timestamp_exit_ns = 123456789;
    return record;
}

template <typename Record>
wsl::audit::DecodedRecord DecodeSecurity(const Record& record)
{
    return wsl::audit::DecodeRecord({reinterpret_cast<const std::byte*>(&record), sizeof(record)}, false);
}

void SecurityDecoderTests()
{
    using namespace wsl::audit;
    NamespaceInspectionTests();
    for (const auto syscall : {__NR_setuid, __NR_setgid, __NR_setresgid, __NR_init_module, __NR_finit_module})
    {
        auto record = SecurityRecord<event_regular_s>(event_type_regular, syscall);
        record.base.a[0] = 1001;
        record.base.a[1] = 1002;
        record.base.a[2] = 1003;
        record.base.return_code = -EPERM;
        const auto decoded = DecodeSecurity(record);
        Check(decoded.status == DecodeStatus::Security, "Supported regular event rejected");
        ValidateSecurityEvent(decoded.message);
        SecurityObservationData(decoded.message);
        Check(decoded.message["result"] == -EPERM && decoded.message["outcome"] == "failure", "Failure result lost");
        if (syscall == __NR_setuid)
        {
            Check(decoded.message["fields"]["oldGid"] == 62001U, "Unchanged old GID was not preserved");
            Check(decoded.message["fields"]["oldUid"].is_null(), "Old changing UID was fabricated");
        }
        if (syscall == __NR_setgid || syscall == __NR_setresgid)
        {
            Check(decoded.message["fields"]["oldUid"] == 63001U, "Unchanged old UID was not preserved");
        }
        if (syscall == __NR_init_module || syscall == __NR_finit_module)
        {
            Check(decoded.message["fields"]["parameters"].is_null(), "A user pointer became module parameters");
        }
        record.base.status = STATUS_CRED | STATUS_RC;
        const auto missing = DecodeSecurity(record);
        Check(missing.message["uid"].is_null() && missing.message["result"].is_null(), "Missing data was reported valid");
    }
    auto fork = SecurityRecord<event_fork_process_s>(event_type_fork_process, __NR_fork);
    fork.base.enriched = 1;
    fork.base.tid = fork.base.pid;
    fork.base.pid2 = 42;
    fork.base.a[0] = UINT64_MAX;
    auto decoded = DecodeSecurity(fork);
    Check(decoded.status == DecodeStatus::Security, "Process creation rejected");
    Check(decoded.message["fields"]["parentPid"] == 42U, "Wrong parent PID");
    Check(
        decoded.message["fields"]["flags"].is_null() && decoded.message["fields"]["parentTid"].is_null(),
        "Synthetic fork flags/TID were mistaken for real syscall data");
    Check(decoded.message["result"].is_null() && decoded.message["outcome"] == "success", "Synthetic result misrepresented");
    fork.base.return_code = -EAGAIN;
    fork.base.uid = 0;
    fork.base.euid = INVALID_CRED;
    auto creation = ProcessCreatedData(DecodeSecurity(fork).message);
    Check(
        creation["outcome"] == "success" && creation["uid"]["value"] == 0U && creation["euid"]["value"].is_null(),
        "Synthetic return code or credential sentinel changed creation semantics");
    fork.base.tid++;
    Check(DecodeSecurity(fork).status == DecodeStatus::Invalid, "Thread-shaped creation record accepted");
    fork.base.tid = fork.base.pid;
    fork.base.enriched = 0;
    Check(DecodeSecurity(fork).status == DecodeStatus::Invalid, "Non-enriched creation record accepted");
    fork.base.enriched = 1;
    fork.base.pid = fork.base.tid = 0;
    Check(DecodeSecurity(fork).status == DecodeStatus::Invalid, "Zero creation identity accepted");
    fork.kthread = true;
    Check(DecodeSecurity(fork).status == DecodeStatus::Ignored, "Kernel thread was logged as user process");

    for (const auto syscall : {__NR_socket, __NR_bind, __NR_connect, __NR_accept, __NR_accept4})
    {
        auto socket = SecurityRecord<event_socket_s>(event_type_socket, syscall);
        socket.base.a[0] = 7;
        socket.base.a[2] = 6;
        socket.socket_identity.valid = SOCKET_IDENTITY_VALID_INODE | SOCKET_IDENTITY_VALID_FAMILY | SOCKET_IDENTITY_VALID_TYPE;
        socket.socket_identity.inode = 123456;
        socket.socket_identity.family = AF_INET;
        socket.socket_identity.type = SOCK_STREAM;
        socket.socket_op.addr.sin_family = AF_INET;
        socket.socket_op.addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socket.socket_op.addr.sin_port = htons(8080);
        decoded = DecodeSecurity(socket);
        Check(decoded.status == DecodeStatus::Security, "Socket event rejected");
        Check(decoded.message["fields"]["protocol"].is_null(), "Socket type was guessed to be protocol");
        Check(decoded.message["fields"]["socketInode"] == 123456U, "Socket identity lost");
        SecurityObservationData(decoded.message);
        if (syscall == __NR_socket)
        {
            Check(decoded.message["fields"]["remoteIp"].is_null(), "Socket creation published a stale endpoint");
            socket.base.status = STATUS_NOARGS;
            const auto missing = DecodeSecurity(socket);
            Check(missing.message["fields"]["requestedProtocol"].is_null(), "Missing socket arguments became protocol");
            socket.base.status = 0;
        }
        else
        {
            socket.base.status = STATUS_NOARGS;
            const auto missing = DecodeSecurity(socket);
            Check(missing.message["fields"]["socketFd"].is_null(), "Missing socket arguments became an FD");
            SecurityObservationData(missing.message);
            socket.base.status = 0;
        }
        if (syscall == __NR_bind)
        {
            Check(
                decoded.message["fields"]["requestedPort"] == 8080U && decoded.message["fields"]["localPort"].is_null(),
                "Requested bind port was presented as final socket port");
        }
        if (syscall == __NR_connect)
        {
            socket.base.return_code = -EINPROGRESS;
            decoded = DecodeSecurity(socket);
            Check(decoded.message["outcome"] == "pending", "Nonblocking connect was called a completed failure");
        }
        ++socket.begin.size;
        Check(DecodeSecurity(socket).status == DecodeStatus::Invalid, "Malformed socket size accepted");
    }
    auto names = SecurityRecord<event_container_namespace_s>(event_type_container_namespace, __NR_unshare);
    names.container_namespace_op.types = event_namespace_type_mnt;
    names.container_namespace_op.mnt_ns_id = 42;
    decoded = DecodeSecurity(names);
    Check(
        decoded.status == DecodeStatus::Security && decoded.message["fields"]["operationMountNamespace"] == 42U,
        "Container namespace identity lost");
    auto legacyNames = SecurityRecord<event_namespace_s>(event_type_namespace, __NR_setns);
    legacyNames.namespace_op.types = event_namespace_type_mnt;
    legacyNames.mnt_ns_id = 43;
    legacyNames.namespace_op.mnt.id = 44;
    decoded = DecodeSecurity(legacyNames);
    Check(
        decoded.status == DecodeStatus::Security && decoded.message["fields"]["previousMountNamespace"] == 43U,
        "Namespace transition origin lost");
    event_dns_rec dns{};
    dns.begin = {CODE_BYTES, VERSION, event_type_dns_uprobe, sizeof(dns)};
    dns.end.code_bytes_end = CODE_BYTES;
    dns.dns_op.pid = 123;
    dns.dns_op.tid = 124;
    dns.dns_op.pid_ns_id = 5678;
    std::strcpy(dns.dns_op.hostname, "example.test");
    decoded = DecodeSecurity(dns);
    Check(decoded.status == DecodeStatus::Security && decoded.message["uid"].is_null(), "DNS credentials fabricated");
    Check(!SecurityObservationData(decoded.message).contains("uid"), "Unsupported DNS credential became a log placeholder");
    dns.dns_op.status = 1;
    dns.dns_op.return_code = 12;
    decoded = DecodeSecurity(dns);
    Check(decoded.message["outcome"] == "failure" && decoded.message["result"] == 12, "DNS error semantics lost");
    dns.dns_op.ipv4_count = MAX_IPV4_COUNT + 1;
    Check(DecodeSecurity(dns).status == DecodeStatus::Invalid, "Oversized DNS address list accepted");
    for (const auto syscall : {__NR_mmap, __NR_ptrace, __NR_clock_settime, __NR_mount})
    {
        const auto unsupported = SecurityRecord<event_regular_s>(event_type_regular, syscall);
        Check(DecodeSecurity(unsupported).status == DecodeStatus::Ignored, "Unsupported or vNext operation was enabled");
    }
}

void LifecycleTest(std::string input, bool expectSuccess, const char* expectedReason = nullptr, bool durationOnly = false)
{
    int inputPipe[2]{};
    int outputPipe[2]{};
    Check(pipe(inputPipe) == 0, "Input pipe creation failed");
    Check(pipe(outputPipe) == 0, "Output pipe creation failed");
    signal(SIGPIPE, SIG_IGN);
    std::thread feeder([&] {
        size_t offset = 0;
        while (offset < input.size())
        {
            const auto count = write(inputPipe[1], input.data() + offset, input.size() - offset);
            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            if (count <= 0)
            {
                break;
            }
            offset += static_cast<size_t>(count);
        }
        if (durationOnly)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
        close(inputPipe[1]);
    });
    const wsl::audit::CollectorOptions options{"/deliberately/missing/package", std::chrono::seconds(1)};
    const auto result = wsl::audit::RunCollector(options, inputPipe[0], outputPipe[1]);
    close(inputPipe[0]);
    feeder.join();
    close(outputPipe[1]);
    std::string output;
    char buffer[4096];
    ssize_t count{};
    while ((count = read(outputPipe[0], buffer, sizeof(buffer))) > 0)
    {
        output.append(buffer, static_cast<size_t>(count));
    }
    close(outputPipe[0]);
    Check((result == 0) == expectSuccess, "Collector exit code incorrect");
    bool ready = false;
    bool stopped = false;
    bool error = false;
    size_t offset = 0;
    while (offset < output.size())
    {
        const auto newline = output.find('\n', offset);
        Check(newline != std::string::npos, "Unterminated protocol output");
        const auto frame = wsl::audit::ParseMessage(std::string_view(output).substr(offset, newline - offset));
        if (frame["kind"] == "ready")
        {
            ready = true;
            Check(
                frame["collectionEnabled"] == false && frame["loadedVariant"] == "" && frame["hooksAttachCode"] == 0 &&
                    frame["status"] == "disabled" && frame["pidNamespace"].is_number_unsigned(),
                "Disabled policy loaded eBPF");
        }
        else if (frame["kind"] == "stopped")
        {
            stopped = true;
            Check(frame["exitCode"] == (expectSuccess ? 0 : 1), "Stopped exit code incorrect");
            Check(frame["processEvents"] == 0, "Disabled collector emitted a process");
            if (expectedReason)
            {
                Check(frame["reason"] == expectedReason, "Wrong stop reason");
            }
        }
        else if (frame["kind"] == "error")
        {
            error = true;
        }
        else
        {
            throw std::runtime_error("Unexpected protocol frame");
        }
        offset = newline + 1;
    }
    Check(stopped && (expectSuccess ? ready && !error : error), "Missing lifecycle/error frames");
}

void DuplexSocketTest()
{
    int sockets[2]{};
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "Duplex socket creation failed");
    const auto input = wsl::audit::EncodeMessage(wsl::audit::Configuration({false, true, "test-override"})) +
                       wsl::audit::EncodeMessage({{"version", wsl::audit::c_protocolVersion}, {"kind", "stop"}});
    Check(
        write(sockets[0], input.data(), input.size()) == static_cast<ssize_t>(input.size()),
        "Duplex socket configuration write failed");
    const wsl::audit::CollectorOptions options{"/deliberately/missing/package", std::chrono::seconds(1)};
    const auto result = wsl::audit::RunCollector(options, sockets[1], sockets[1]);
    close(sockets[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    ssize_t count{};
    while ((count = read(sockets[0], buffer.data(), buffer.size())) > 0)
    {
        output.append(buffer.data(), static_cast<size_t>(count));
    }
    close(sockets[0]);
    Check(count == 0 && result == 0, "Duplex socket did not close cleanly");
    std::istringstream frames(output);
    std::string line;
    Check(static_cast<bool>(std::getline(frames, line)), "Duplex socket READY missing");
    const auto ready = wsl::audit::ParseMessage(line);
    Check(
        ready["kind"] == "ready" && ready["collectionEnabled"] == false && ready["tracefsPrivateMount"] == false,
        "Disabled duplex collector changed tracing state");
    Check(static_cast<bool>(std::getline(frames, line)), "Duplex socket STOPPED missing");
    const auto stopped = wsl::audit::ParseMessage(line);
    Check(stopped["kind"] == "stopped" && stopped["exitCode"] == 0, "Duplex socket STOPPED invalid");
    Check(!std::getline(frames, line), "Unexpected duplex socket frame");
}

void CollectionPlanTests()
{
    using namespace wsl::audit;
    for (unsigned int mask = 0; mask < 32; ++mask)
    {
        const AuditPolicy policy{(mask & 1) != 0, false, "test-override", (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0, (mask & 16) != 0};
        const auto plan = BuildCollectionPlan(policy);
        const bool network = policy.networkSuccess || policy.networkFailure;
        const size_t expectedCount =
            (policy.processCreation ? 2 : 0) + (network ? 5 : 0) + (policy.moduleLoad ? 2 : 0) + (policy.privilegeTransition ? 3 : 0);
        Check(plan.syscalls.size() == expectedCount, "Disabled family retained kernel subscriptions");
        Check(((plan.features & ENABLE_FEATURE_UPROBE_DNS_ENRICHMENT) != 0) == network, "DNS hooks not gated by network policy");
        Check(((plan.runtimeFlags & FEATURE_SCHED_PROCESS_FORK) != 0) == policy.processCreation, "Fork hooks not gated");
        Check(((plan.runtimeFlags & FEATURE_DNS_FAILURE_EVENTS) != 0) == policy.networkFailure, "DNS failure flag not gated");
        if (!policy.AnyEnabled())
        {
            Check(plan.features == 0 && plan.runtimeFlags == 0, "Disabled collection retained feature flags");
        }
        for (const auto& syscall : plan.syscalls)
        {
            const bool networkCall = syscall.number == __NR_socket || syscall.number == __NR_bind || syscall.number == __NR_connect ||
                                     syscall.number == __NR_accept || syscall.number == __NR_accept4;
            Check(syscall.successOnly == (!networkCall || !policy.networkFailure), "Wrong kernel success-only filter");
            Check(syscall.number != __NR_unshare && syscall.number != __NR_setns, "Deferred namespace hook subscribed");
            Check(
                std::count_if(
                    plan.syscalls.begin(), plan.syscalls.end(), [&](const auto& other) { return other.number == syscall.number; }) == 1,
                "Duplicate syscall subscription");
        }
        const auto subscribed = [&](int number) {
            return std::any_of(plan.syscalls.begin(), plan.syscalls.end(), [&](const auto& call) { return call.number == number; });
        };
        Check(
            subscribed(__NR_execve) == policy.processCreation && subscribed(__NR_fork) == policy.processCreation,
            "Process hooks mismatch");
        Check(
            subscribed(__NR_init_module) == policy.moduleLoad && subscribed(__NR_finit_module) == policy.moduleLoad,
            "Module hooks mismatch");
        for (const auto call : {__NR_setuid, __NR_setgid, __NR_setresgid})
        {
            Check(subscribed(call) == policy.privilegeTransition, "Privilege hooks mismatch");
        }
        for (const auto call : {__NR_socket, __NR_bind, __NR_connect, __NR_accept, __NR_accept4})
        {
            Check(subscribed(call) == network, "Network hooks mismatch");
        }
    }
}

} // namespace

int main()
{
    try
    {
        using namespace wsl::audit;
        DecoderTests();
        SecurityDecoderTests();
        CollectionPlanTests();
        auto disabled = wsl::audit::EncodeMessage(wsl::audit::Configuration({false, true, "test-override"}));
        const auto stop = wsl::audit::EncodeMessage({{"version", wsl::audit::c_protocolVersion}, {"kind", "stop"}});
        LifecycleTest(disabled + stop, true, "stop");
        LifecycleTest(disabled, true, "eof");
        LifecycleTest(disabled, true, "duration", true);
        LifecycleTest("not json\n", false);
        LifecycleTest(disabled + "{\"version\":1,\"kind\":\"wrong\"}\n", false);
        LifecycleTest(disabled + "{\"version\":1,\"kind\":\"stop\"}", false);
        LifecycleTest(std::string(wsl::audit::c_maxMessageSize + 1, 'x') + "\n", false);
        const auto enabled = wsl::audit::EncodeMessage(wsl::audit::Configuration({true, false, "test-override"}));
        LifecycleTest(enabled, false);
        for (const auto& policy :
             {AuditPolicy{false, false, "test-override", true, false},
              AuditPolicy{false, false, "test-override", false, true},
              AuditPolicy{false, false, "test-override", false, false, true},
              AuditPolicy{false, false, "test-override", false, false, false, true}})
        {
            LifecycleTest(EncodeMessage(Configuration(policy)), false);
        }
        DuplexSocketTest();
        std::cout << "Collector decoder, redaction, and lifecycle tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Collector tests failed: " << error.what() << '\n';
        return 1;
    }
}
