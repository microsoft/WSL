// Copyright (C) Microsoft Corporation. All rights reserved.
#include <linux/unistd.h>

#include "SecCompDispatcher.h"
#include "common.h"
#include "Syscall.h"
#include "SyscallError.h"

static int seccomp(unsigned int operation, unsigned int flags, void* args, const std::source_location& source = std::source_location::current())
{
    int result = syscall(__NR_seccomp, operation, flags, args);
    if (result < 0)
    {
        auto error = errno;
        std::stringstream argString;
        detail::PrettyPrintArguments(argString, operation, flags, args);
        throw SyscallError("seccomp", argString.str(), error, source);
    }

    return result;
}

SecCompDispatcher::SecCompDispatcher(int m_NotifyFd) : m_notifyFd(m_NotifyFd)
{
    seccomp(SECCOMP_GET_NOTIF_SIZES, 0, &m_notificationSizes);

    m_worker = std::thread([this]() { Run(); });
}

SecCompDispatcher::~SecCompDispatcher()
{
    m_shutdown.reset();
    m_worker.join();
}

/**
 * @brief Poll for notifications from seccomp and dispatch them a handler.
 *
 */
void SecCompDispatcher::Run()
{
    UtilSetThreadName("SecCompDispatcher");

    // Create a pipe to signal the thread to stop.
    int pipes[2];
    Syscall(pipe2, pipes, 0);
    m_shutdown = pipes[1];
    wil::unique_fd terminate = pipes[0];
    auto wait_for_fd = [&terminate](int fd, int event) -> bool {
        struct pollfd poll_fds[2];
        poll_fds[0] = {.fd = fd, .events = event, .revents = 0};
        poll_fds[1] = {.fd = terminate.get(), .events = POLLIN, .revents = 0};
        for (;;)
        {
            int return_value = SyscallInterruptable(poll, poll_fds, ARRAY_SIZE(poll_fds), -1);
            if (return_value < 0)
            {
                continue;
            }
            else if (return_value == 0)
            {
                continue;
            }
            else if (poll_fds[1].revents)
            {
                return false;
            }
            else if (poll_fds[0].revents & event)
            {
                return true;
            }
        }
    };
    std::vector<uint8_t> notification_buffer(m_notificationSizes.seccomp_notif);
    std::vector<std::uint8_t> response_buffer(m_notificationSizes.seccomp_notif_resp);
    assert(m_notificationSizes.seccomp_notif_resp >= sizeof(seccomp_notif_resp));
    for (;;)
    {
        if (!wait_for_fd(m_notifyFd.get(), POLLIN))
        {
            break;
        }

        // Clear the buffers to make the 5.15 kernel happy.
        notification_buffer.clear();
        notification_buffer.resize(m_notificationSizes.seccomp_notif);

        auto* callInfo = reinterpret_cast<seccomp_notif*>(notification_buffer.data());
        try
        {
            Syscall(ioctl, m_notifyFd.get(), SECCOMP_IOCTL_NOTIF_RECV, callInfo);
        }
        catch (const SyscallError& e)
        {
            if (e.GetErrno() == ENOENT)
            {
                // The target thread was killed by a signal as the notification information was being generated,
                // or the target's (blocked) system call was interrupted by a signal handler.
                GNS_LOG_INFO("SECCOMP_IOCTL_NOTIF_RECV failed with ENOENT");
                continue;
            }

            throw;
        }
        int result = 0;
        GNS_LOG_INFO(
            "Notified for arch {:X} syscall {} with id {}lu for pid {} with args ({}lX, {}lX, {}lX, {}lX, {}lX, "
            "{}lX)",
            callInfo->data.arch,
            callInfo->data.nr,
            callInfo->id,
            callInfo->pid,
            callInfo->data.args[0],
            callInfo->data.args[1],
            callInfo->data.args[2],
            callInfo->data.args[3],
            callInfo->data.args[4],
            callInfo->data.args[5]);

        auto handler = m_handlers.find(callInfo->data.nr);

        try
        {
            if (handler != m_handlers.end())
            {
                result = handler->second(callInfo);
            }
        }
        catch (std::exception& e)
        {
            GNS_LOG_ERROR("Dispatch of call failed, {}", e.what());
        }

        response_buffer.clear();
        response_buffer.resize(m_notificationSizes.seccomp_notif_resp);

        auto* resultInfo = reinterpret_cast<seccomp_notif_resp*>(response_buffer.data());
        resultInfo->id = callInfo->id;
        resultInfo->error = -result;
        resultInfo->val = 0;
        resultInfo->flags = result == 0 ? SECCOMP_USER_NOTIF_FLAG_CONTINUE : 0;

        GNS_LOG_INFO("Responding to notification with id {}lu for pid {}, result {}", callInfo->id, callInfo->pid, result);
        try
        {
            Syscall(ioctl, m_notifyFd.get(), SECCOMP_IOCTL_NOTIF_SEND, resultInfo);
        }
        catch (std::exception& e)
        {
            GNS_LOG_ERROR("Failed to respond to notification with id {}lu for pid {}, {}", callInfo->id, callInfo->pid, e.what());
        }
    }
}

bool SecCompDispatcher::ValidateCookie(uint64_t id) noexcept
{
    try
    {
        // If the cookie is not valid, the ioctl will return < 0 and the call below will throw
        Syscall(ioctl, m_notifyFd.get(), SECCOMP_IOCTL_NOTIF_ID_VALID, &id);
        return true;
    }
    catch (std::exception& e)
    {
        return false;
    }
}

void SecCompDispatcher::RegisterHandler(int SysCallNr, const std::function<int(seccomp_notif*)>& Handler)
{
    m_handlers[SysCallNr] = Handler;
}

void SecCompDispatcher::UnregisterHandler(int SysCallNr)
{
    m_handlers.erase(SysCallNr);
}

std::optional<std::vector<gsl::byte>> SecCompDispatcher::ReadProcessMemory(uint64_t Cookie, pid_t Pid, size_t Address, size_t Length) noexcept
{
    try
    {
        std::vector<gsl::byte> targetMemory(Length);
        const std::string path = std::format("/proc/{}/mem", Pid);
        wil::unique_fd mem(Syscall(open, path.c_str(), O_RDWR));

        // PID reuse can cause a TOCTOU race here, so validate that the notification is still
        // valid to make sure that the above fd points to the right process
        if (!ValidateCookie(Cookie))
        {
            throw RuntimeErrorWithSourceLocation(std::format("Invalid cookie {}", Cookie));
        }

        Syscall(lseek64, mem.get(), Address, SEEK_SET);
        if (Syscall(read, mem.get(), targetMemory.data(), targetMemory.size()) != targetMemory.size())
        {
            throw RuntimeErrorWithSourceLocation(std::format("Couldn't read the whole call address with error {}", errno));
        }

        // Based on https://man7.org/linux/man-pages/man2/seccomp_unotify.2.html (example in function getTargetPathname),
        // it's possible that right before reading the process memory, the intercepted system call was interrupted by a signal and
        // the memory we read may no longer be associated with that system call
        //
        // We need to validate the cookie again to make sure the seccomp notification is still valid after we read the process memory
        if (!ValidateCookie(Cookie))
        {
            throw RuntimeErrorWithSourceLocation(std::format("Invalid cookie {}", Cookie));
        }

        return targetMemory;
    }
    catch (std::exception& e)
    {
        GNS_LOG_ERROR("Failed to read process memory for pid {}, cookie {}u, {}", Pid, Cookie, e.what());
        return std::nullopt;
    }
}
