// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

/*
 * This class contains the implementation for the Syscall() wrapper.
 * This wrapper automatically throws a detailed exception if the
 * underlying call fails.
 * Example exception message:
 *  what():  Exception thrown by NetlinkChannel in ./NetlinkChannel.hxx:18 :
 *  socket(16, 1248, 0) failed with errno=22 (Invalid argument)
 */

#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <arpa/inet.h>
#include <type_traits>
#include <sys/xattr.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sstream>
#include <map>
#include <cassert>
#include "SyscallError.h"
#include "Utils.h"

#define X(Method) {(void*)&Method, #Method}

namespace detail {
static const std::map<const void*, const char*> syscalls{
    X(bind),     X(ioctl),   X(socket),        X(inet_pton), X(send),       X(sendto), X(recv),   X(sendto),
    X(recvfrom), X(recvmsg), X(read),          X(lseek),     X(open),       X(prctl),  X(fork),   X(execl),
    X(poll),     X(pipe),    X(socketpair),    X(readlink),  X(getxattr),   X(dup),    X(write),  X(pipe2),
    X(syscall),  X(stat),    X(epoll_create1), X(epoll_ctl), X(epoll_wait), X(listen), X(accept4)};
#undef X

inline std::string ArgumentToString(const std::nullptr_t&)
{
    return "nullptr";
}

template <typename T>
std::string ArgumentToString(T* arg)
{
    std::stringstream output;
    if constexpr (std::is_class_v<T>)
    {
        utils::FormatBinary(output, arg, sizeof(*arg));
    }
    else
    {
        output << utils::BytesToHex(&arg, sizeof(arg), "");
    }

    return output.str();
}

template <typename T>
std::string ArgumentToString(T arg)
{
    return std::to_string(arg);
}

inline void PrettyPrintArguments(std::ostream&)
{
}

template <typename T>
void PrettyPrintArguments(std::ostream& out, T first)
{
    out << ArgumentToString(first);
}

template <typename T, typename... Args>
void PrettyPrintArguments(std::ostream& out, T first, Args... args)
{
    out << ArgumentToString(first) << ", ";
    PrettyPrintArguments(out, std::forward<Args>(args)...);
}
} // namespace detail

template <typename Routine, typename... Args>
typename std::invoke_result<Routine, Args...>::type _Syscall(const std::source_location& source, Routine routine, Args... args)
{
    const auto call = detail::syscalls.find(reinterpret_cast<void*>(routine));
    assert(call != detail::syscalls.end());

    auto result = routine(std::forward<Args>(args)...);
    if (result >= 0)
    {
        return result;
    }

    const int savedErrno = errno;

    std::stringstream argString;
    detail::PrettyPrintArguments(argString, std::forward<Args>(args)...);

    throw SyscallError(call->second, argString.str(), savedErrno, source);
}

template <typename Routine, typename... Args>
typename std::invoke_result<Routine, Args...>::type _SyscallInterruptable(const std::source_location& source, Routine routine, Args... args)
{
    const auto call = detail::syscalls.find(reinterpret_cast<void*>(routine));
    assert(call != detail::syscalls.end());

    auto result = routine(std::forward<Args>(args)...);
    if (result >= 0)
    {
        return result;
    }

    const int savedErrno = errno;
    if (savedErrno == EINTR)
    {
        return result;
    }

    std::stringstream argString;
    detail::PrettyPrintArguments(argString, std::forward<Args>(args)...);

    throw SyscallError(call->second, argString.str(), savedErrno, source);
}
