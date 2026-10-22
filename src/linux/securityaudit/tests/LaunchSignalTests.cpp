// Copyright (C) Microsoft Corporation. All rights reserved.

#include "SecurityAuditLaunch.h"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

class Descriptor
{
public:
    explicit Descriptor(int value = -1) : m_value(value)
    {
        if (value < 0)
        {
            throw std::runtime_error("Cannot create test socket");
        }
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

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void WaitReadable(int fd)
{
    pollfd descriptor{fd, POLLIN, 0};
    int result{};
    do
    {
        result = poll(&descriptor, 1, 30000);
    } while (result < 0 && errno == EINTR);
    Check(result > 0 && (descriptor.revents & (POLLIN | POLLHUP)), "Timed out waiting for test socket");
}

char Receive(int fd)
{
    WaitReadable(fd);
    char value{};
    Check(read(fd, &value, sizeof(value)) == sizeof(value), "Listener disconnected unexpectedly");
    return value;
}

void Send(int fd, char value)
{
    // The parent models Windows, where a disconnected write does not raise POSIX SIGPIPE.
    Check(send(fd, &value, sizeof(value), MSG_NOSIGNAL) == sizeof(value), "Cannot send test request");
}

int Accept(int fd)
{
    WaitReadable(fd);
    return accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
}

void RunChild(int listener, int control)
{
    Check(IgnoreSecurityAuditListenerSigpipe() == 0, "Cannot configure production listener SIGPIPE policy");
    Descriptor first(Accept(listener));
    Check(Receive(first.Get()) == 'Q', "First request missing");
    Check(Receive(control) == 'C', "Cancellation synchronization missing");
    const char reply = 'A';
    errno = 0;
    Check(write(first.Get(), &reply, sizeof(reply)) == -1 && errno == EPIPE, "Cancelled reply did not report EPIPE");
    first.Close();
    Send(control, 'E');

    Descriptor second(Accept(listener));
    Check(Receive(second.Get()) == 'Q', "Second request missing");
    Check(write(second.Get(), &reply, sizeof(reply)) == sizeof(reply), "Listener could not reply to the next request");
}

int Reap(pid_t child)
{
    int status{};
    pid_t result{};
    do
    {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    Check(result == child, "Cannot reap test listener");
    if (WIFSIGNALED(status))
    {
        throw std::runtime_error("Listener terminated by signal " + std::to_string(WTERMSIG(status)));
    }
    Check(WIFEXITED(status), "Listener did not exit normally");
    return WEXITSTATUS(status);
}

} // namespace

int main()
{
    pid_t child = -1;
    try
    {
        struct sigaction original{};
        original.sa_handler = SIG_DFL;
        Check(
            sigemptyset(&original.sa_mask) == 0 && sigaction(SIGPIPE, &original, nullptr) == 0,
            "Cannot set parent signal baseline");
        sigset_t mask{};
        Check(
            sigemptyset(&mask) == 0 && sigaddset(&mask, SIGPIPE) == 0 && sigprocmask(SIG_UNBLOCK, &mask, nullptr) == 0,
            "Cannot unblock SIGPIPE for the regression");
        Descriptor listener(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto name = "wsl-audit-launch-signal-" + std::to_string(getpid()) + "-" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        Check(name.size() + 1 < sizeof(address.sun_path), "Test socket name is too long");
        std::memcpy(address.sun_path + 1, name.data(), name.size());
        const auto addressSize = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
        Check(
            bind(listener.Get(), reinterpret_cast<const sockaddr*>(&address), addressSize) == 0 && listen(listener.Get(), 2) == 0,
            "Cannot listen on private abstract test socket");
        int control[2]{};
        Check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) == 0, "Cannot create synchronization socket");
        Descriptor parentControl(control[0]), childControl(control[1]);
        child = fork();
        Check(child >= 0, "Cannot fork test listener");
        if (child == 0)
        {
            parentControl.Close();
            try
            {
                RunChild(listener.Get(), childControl.Get());
                _exit(0);
            }
            catch (const std::exception& error)
            {
                std::fprintf(stderr, "Child: %s\n", error.what());
                _exit(1);
            }
        }
        childControl.Close();
        listener.Close();

        Descriptor first(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        Check(connect(first.Get(), reinterpret_cast<const sockaddr*>(&address), addressSize) == 0, "Cannot connect first request");
        Send(first.Get(), 'Q');
        first.Close();
        Send(parentControl.Get(), 'C');
        Check(Receive(parentControl.Get()) == 'E', "Cancelled reply was not handled");

        Descriptor second(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        Check(
            connect(second.Get(), reinterpret_cast<const sockaddr*>(&address), addressSize) == 0,
            "Listener no longer accepts requests");
        Send(second.Get(), 'Q');
        Check(Receive(second.Get()) == 'A', "Second request was not acknowledged");
        const auto result = Reap(child);
        child = -1;
        Check(result == 0, "Test listener failed");
        struct sigaction parentAction{};
        Check(
            sigaction(SIGPIPE, nullptr, &parentAction) == 0 && parentAction.sa_handler == SIG_DFL,
            "Child signal policy unexpectedly changed the parent");
        std::puts("PASS: cancelled write returned EPIPE, listener accepted the next request, parent SIGPIPE remained default");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        if (child > 0)
        {
            try
            {
                (void)Reap(child);
            }
            catch (const std::exception& childError)
            {
                std::fprintf(stderr, "Child status: %s\n", childError.what());
            }
        }
        return 1;
    }
}
