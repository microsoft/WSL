// Copyright (C) Microsoft Corporation. All rights reserved.
#pragma once

#include <type_traits>
#include <source_location>

template <typename Routine, typename... Args>
typename std::invoke_result<Routine, Args...>::type _Syscall(const std::source_location& source, Routine routine, Args... args);
#define Syscall(...) _Syscall(std::source_location::current(), __VA_ARGS__)

template <typename Routine, typename... Args>
typename std::invoke_result<Routine, Args...>::type _SyscallInterruptable(const std::source_location& source, Routine routine, Args... args);
#define SyscallInterruptable(...) _SyscallInterruptable(std::source_location::current(), __VA_ARGS__)

#include "Syscall.hxx"
