// Copyright (C) Microsoft Corporation. All rights reserved.

#include <iostream>
#include <sys/syscall.h>
#include "ebpf_arch_macros.h"

int main()
{
    // The public package omits SYSCALL_FILE. Generate the names and argument counts
    // used by our collection plan from that package's architecture definitions.
#define SYSCALL(name, count) std::cout << __NR_##name << "=" #name "," #count "\n"
    SYSCALL(execve, 3);
    SYSCALL(execveat, 5);
    SYSCALL(fork, 0);
    SYSCALL(clone, 5);
    SYSCALL(clone3, 2);
    SYSCALL(exit, 1);
    SYSCALL(exit_group, 1);
    SYSCALL(socket, 3);
    SYSCALL(bind, 3);
    SYSCALL(connect, 3);
    SYSCALL(accept, 3);
    SYSCALL(accept4, 4);
    SYSCALL(init_module, 3);
    SYSCALL(finit_module, 3);
    SYSCALL(setuid, 1);
    SYSCALL(setgid, 1);
    SYSCALL(setresgid, 3);
#undef SYSCALL
    return std::cout.good() ? 0 : 1;
}
