// Copyright (C) Microsoft Corporation. All rights reserved.

#include "Protocol.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <set>
#include <thread>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

class Descriptor
{
public:
    explicit Descriptor(int value = -1) : m_value(value)
    {
    }
    ~Descriptor()
    {
        Close();
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int Get() const
    {
        return m_value;
    }
    void Close()
    {
        if (m_value >= 0)
        {
            close(m_value);
            m_value = -1;
        }
    }

private:
    int m_value;
};

void Send(int fd, const nlohmann::json& frame)
{
    const auto bytes = wsl::audit::EncodeMessage(frame);
    size_t offset{};
    while (offset < bytes.size())
    {
        const auto size = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (size < 0 && errno == EINTR)
        {
            continue;
        }
        if (size <= 0)
        {
            throw std::runtime_error("Cannot send collector configuration/stop");
        }
        offset += static_cast<size_t>(size);
    }
}

class Frames
{
public:
    explicit Frames(int fd) : m_fd(fd)
    {
    }
    nlohmann::json Next(Clock::time_point deadline)
    {
        while (Clock::now() < deadline)
        {
            const auto newline = m_pending.find('\n');
            if (newline != std::string::npos)
            {
                auto result = wsl::audit::ParseMessage(m_pending.substr(0, newline));
                m_pending.erase(0, newline + 1);
                if (result.at("kind") == "error")
                {
                    throw std::runtime_error(result.at("message").get<std::string>());
                }
                return result;
            }
            if (m_pending.size() > wsl::audit::c_maxMessageSize)
            {
                throw std::runtime_error("Oversized collector frame");
            }
            pollfd descriptor{m_fd, POLLIN, 0};
            const auto ready = poll(&descriptor, 1, 100);
            if (ready < 0 && errno == EINTR)
            {
                continue;
            }
            if (ready < 0 || (descriptor.revents & (POLLERR | POLLNVAL)))
            {
                throw std::runtime_error("Collector output poll failed");
            }
            if (!ready)
            {
                continue;
            }
            std::array<char, 4096> buffer{};
            const auto count = read(m_fd, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            if (count <= 0)
            {
                throw std::runtime_error("Collector closed before the required event");
            }
            m_pending.append(buffer.data(), static_cast<size_t>(count));
        }
        throw std::runtime_error("Timed out waiting for real BPF collector output");
    }

private:
    int m_fd;
    std::string m_pending;
};

int Wait(pid_t pid)
{
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < deadline)
    {
        int status{};
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid)
        {
            if (!WIFEXITED(status))
            {
                throw std::runtime_error("Child did not exit normally");
            }
            return WEXITSTATUS(status);
        }
        if (result < 0 && errno != EINTR)
        {
            throw std::runtime_error("Cannot reap collector/test workload");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("Child did not exit within 30 seconds; no forced process termination attempted");
}

void CheckEmptyKernelMountpoints(const std::string& root)
{
    for (const auto* path : {"/proc", "/sys"})
    {
        if (!std::filesystem::is_empty(root + path))
        {
            throw std::runtime_error("Minimal-root proc/sys was pre-mounted or a collector mount escaped its namespace");
        }
    }
}

void CheckPrivateKernelMounts(pid_t collector, const std::string& root)
{
    CheckEmptyKernelMountpoints(root);
    struct stat parentNamespace{}, collectorNamespace{};
    const auto process = "/proc/" + std::to_string(collector);
    if (stat("/proc/self/ns/mnt", &parentNamespace) < 0 || stat((process + "/ns/mnt").c_str(), &collectorNamespace) < 0 ||
        parentNamespace.st_ino == collectorNamespace.st_ino)
    {
        throw std::runtime_error("Collector did not isolate its mount namespace");
    }
    struct statfs proc{}, sys{}, trace{};
    if (statfs((process + "/root/proc").c_str(), &proc) < 0 || proc.f_type != 0x9fa0 || statfs((process + "/root/sys").c_str(), &sys) < 0 ||
        sys.f_type != 0x62656572 || statfs((process + "/root/sys/kernel/tracing").c_str(), &trace) < 0 ||
        trace.f_type != 0x74726163 || access((process + "/root/sys/kernel/btf/vmlinux").c_str(), R_OK) < 0)
    {
        throw std::runtime_error("Collector did not prepare real private procfs/sysfs/tracefs and kernel BTF");
    }
}

void InspectMinimalRoot(const std::string& root)
{
    int output[2]{};
    if (pipe2(output, O_CLOEXEC) < 0)
    {
        throw std::runtime_error("Cannot create namespace inspection pipe");
    }
    Descriptor outputRead(output[0]), outputWrite(output[1]);
    const auto parent = std::to_string(getpid());
    const auto child = fork();
    if (child < 0)
    {
        throw std::runtime_error("Cannot fork namespace inspection helper");
    }
    if (!child)
    {
        if (dup2(outputWrite.Get(), STDOUT_FILENO) < 0 || chroot(root.c_str()) < 0 || chdir("/") < 0)
        {
            _exit(126);
        }
        execl("/wsl-audit-collector", "/wsl-audit-collector", "--inspect-pid", parent.c_str(), nullptr);
        _exit(127);
    }
    outputWrite.Close();
    pollfd descriptor{outputRead.Get(), POLLIN, 0};
    if (poll(&descriptor, 1, 30000) <= 0)
    {
        throw std::runtime_error("Namespace inspection timed out");
    }
    std::array<char, 4096> buffer{};
    const auto count = read(outputRead.Get(), buffer.data(), buffer.size());
    if (count <= 0 || count == static_cast<ssize_t>(buffer.size()) || Wait(child) != 0)
    {
        throw std::runtime_error("Native-root namespace inspection failed");
    }
    const auto identity = nlohmann::json::parse(buffer.data(), buffer.data() + count);
    struct stat pidNamespace{}, mountNamespace{};
    if (stat("/proc/self/ns/pid", &pidNamespace) < 0 || stat("/proc/self/ns/mnt", &mountNamespace) < 0 ||
        identity.at("pid") != static_cast<uint32_t>(getpid()) || identity.at("pidNamespace") != pidNamespace.st_ino ||
        identity.at("mountNamespace") != mountNamespace.st_ino || identity.at("observedAfterNs") == 0)
    {
        throw std::runtime_error("Native-root inspection changed the target namespace identity");
    }
    CheckEmptyKernelMountpoints(root);
}

} // namespace

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--network-workload")
        {
            Descriptor socketFd(socket(AF_INET, SOCK_DGRAM, 0));
            return socketFd.Get() < 0 ? 1 : 0;
        }
        const bool minimalRoot = argc == 5 && std::string_view(argv[3]) == "--minimal-root";
        if (argc != 3 && !minimalRoot)
        {
            throw std::invalid_argument("Usage: audit_live_collector_test <static-collector> <BPF-package-directory>");
        }
        if (minimalRoot)
        {
            CheckEmptyKernelMountpoints(argv[4]);
            InspectMinimalRoot(argv[4]);
        }
        int input[2]{};
        if (pipe2(input, O_CLOEXEC) < 0)
        {
            throw std::runtime_error("Cannot create input pipe");
        }
        Descriptor inputRead(input[0]), inputWrite(input[1]);
        int output[2]{};
        if (pipe2(output, O_CLOEXEC) < 0)
        {
            throw std::runtime_error("Cannot create output pipe");
        }
        Descriptor outputRead(output[0]), outputWrite(output[1]);
        const auto collector = fork();
        if (collector < 0)
        {
            throw std::runtime_error("Cannot start collector");
        }
        if (!collector)
        {
            if (dup2(inputRead.Get(), STDIN_FILENO) < 0 || dup2(outputWrite.Get(), STDOUT_FILENO) < 0)
            {
                _exit(126);
            }
            if (minimalRoot && (chroot(argv[4]) < 0 || chdir("/") < 0))
            {
                _exit(126);
            }
            const auto* collectorPath = minimalRoot ? "/wsl-audit-collector" : argv[1];
            const auto* packagePath = minimalRoot ? "/wsl-security-audit" : argv[2];
            execl(collectorPath, collectorPath, "--package", packagePath, "--duration", "240", nullptr);
            _exit(127);
        }
        inputRead.Close();
        outputWrite.Close();
        wsl::audit::AuditPolicy policy;
        policy.processCreation = true;
        policy.networkSuccess = minimalRoot;
        Send(inputWrite.Get(), wsl::audit::Configuration(policy));
        Frames frames(outputRead.Get());
        const auto ready = frames.Next(Clock::now() + std::chrono::seconds(120));
        if (ready.at("kind") != "ready" || ready.at("status") != "running" || !ready.at("staticLoader").get<bool>() ||
            !ready.at("loadedVariant").get<std::string>().ends_with("_btf_rbuff_enriched_fmap.o"))
        {
            throw std::runtime_error("Real loader did not attach the required BPF variant");
        }
        if (minimalRoot)
        {
            CheckPrivateKernelMounts(collector, argv[4]);
            if (!ready.at("tracefsPrivateMount").get<bool>())
            {
                throw std::runtime_error("Minimal root did not report its private tracefs mount");
            }
            if (ready.at("dnsProbeCount") != 0 || ready.at("dnsCoverage") != "unavailable")
            {
                throw std::runtime_error("Minimal root falsely reported attached DNS probes");
            }
            for (const auto& operation : ready.at("coverage").at("network").at("supported"))
            {
                if (operation == "dns-query")
                {
                    throw std::runtime_error("Minimal root falsely advertised DNS collection");
                }
            }
        }
        const auto workload = fork();
        if (workload < 0)
        {
            throw std::runtime_error("Cannot start real exec workload");
        }
        if (!workload)
        {
            if (minimalRoot)
            {
                execl(argv[0], argv[0], "--network-workload", nullptr);
                _exit(127);
            }
            execl("/bin/true", "true", nullptr);
            _exit(127);
        }
        if (Wait(workload) != 0)
        {
            throw std::runtime_error("Exec workload failed");
        }
        const auto deadline = Clock::now() + std::chrono::seconds(60);
        bool observed{};
        uint32_t rootPid{};
        std::set<uint32_t> networkPids;
        while (!observed || (minimalRoot && !networkPids.contains(rootPid)))
        {
            const auto message = frames.Next(deadline);
            if (message.at("kind") == "process" && message.at("namespacePid") == static_cast<uint32_t>(workload) &&
                message.at("pidNamespace") == ready.at("pidNamespace"))
            {
                observed = true;
                rootPid = message.at("pid").get<uint32_t>();
            }
            else if (
                minimalRoot && message.at("kind") == "security" && message.at("operation") == "socket" &&
                message.at("pidNamespace") == ready.at("pidNamespace"))
            {
                networkPids.insert(message.at("pid").get<uint32_t>());
                if (networkPids.size() > 256)
                {
                    throw std::runtime_error("Unexpected network traffic exceeded test matching bounds");
                }
            }
        }
        Send(inputWrite.Get(), {{"version", wsl::audit::c_protocolVersion}, {"kind", "stop"}});
        inputWrite.Close();
        const auto stopDeadline = Clock::now() + std::chrono::seconds(30);
        nlohmann::json stopped;
        do
        {
            stopped = frames.Next(stopDeadline);
        } while (stopped.at("kind") != "stopped");
        if (stopped.at("exitCode") != 0 || Wait(collector) != 0)
        {
            throw std::runtime_error("Collector did not stop cleanly");
        }
        if (minimalRoot)
        {
            CheckEmptyKernelMountpoints(argv[4]);
        }
        std::puts(
            minimalRoot
                ? "PASS: empty proc/sys root privately mounted kernel filesystems, loaded real BPF, observed exec/socket, "
                  "reported DNS unavailable, stopped cleanly, and left parent mountpoints empty"
                : "PASS: real static loader attached BPF, observed exec, and acknowledged STOPPED");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
