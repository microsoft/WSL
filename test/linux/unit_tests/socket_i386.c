// Copyright (C) Microsoft Corporation. All rights reserved.

#if !defined(__i386__)
#error This helper must be compiled for i386.
#endif

// Local ABI definitions and raw syscalls avoid requiring 32-bit libc headers or
// libraries in the test distro.
#define I386_NR_exit 1
#define I386_NR_write 4
#define I386_NR_socketcall 102
#define I386_NR_poll 168
#define I386_NR_bind 361
#define I386_NR_listen 363

#define SYS_SOCKET 1
#define SYS_BIND 2
#define SYS_LISTEN 4
#define SYS_ACCEPT 5
#define SYS_GETSOCKNAME 6

#define AF_INET 2
#define SOCK_STREAM 1
#define INADDR_ANY 0
#define POLLIN 1
#define EINTR 4
#define STDOUT_FILENO 1

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define EXIT_USAGE 2
#define EXIT_TIMEOUT 124
#define POLL_TIMEOUT_MS 60000
#define LISTEN_BACKLOG 1
#define MAX_PORT 65535

struct SocketAddress
{
    unsigned short family;
    unsigned short port;
    unsigned int address;
    unsigned char padding[8];
};

struct PollDescriptor
{
    int descriptor;
    short events;
    short returnedEvents;
};

static long Syscall(long number, long first, long second, long third)
{
    __asm__ volatile("int $0x80" : "+a"(number) : "b"(first), "c"(second), "d"(third) : "memory", "cc");
    return number;
}

__asm__(".global _start\n_start:\nmov %esp,%eax\nand $-16,%esp\nsub $12,%esp\npush %eax\ncall Entry\n");

__attribute__((noreturn)) static void Exit(int status)
{
    Syscall(I386_NR_exit, status, 0, 0);
    __builtin_unreachable();
}

static long Check(long result)
{
    if (result < 0)
    {
        Exit((int)-result);
    }
    return result;
}

static int Equals(const char* left, const char* right)
{
    while (*left && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

static long Socketcall(int operation, long first, long second, long third)
{
    unsigned long arguments[] = {first, second, third};
    return Check(Syscall(I386_NR_socketcall, operation, (long)arguments, 0));
}

static void Write(const void* buffer, unsigned long length)
{
    const char* position = buffer;
    while (length)
    {
        long written = Syscall(I386_NR_write, STDOUT_FILENO, (long)position, length);
        if (written == -EINTR)
        {
            continue;
        }
        if (Check(written) == 0)
        {
            Exit(EXIT_FAILURE);
        }
        position += written;
        length -= written;
    }
}

static void WaitForInput(int descriptor)
{
    struct PollDescriptor polling = {descriptor, POLLIN, 0};
    if (Check(Syscall(I386_NR_poll, (long)&polling, 1, POLL_TIMEOUT_MS)) == 0)
    {
        Exit(EXIT_TIMEOUT);
    }
    if (!(polling.returnedEvents & POLLIN))
    {
        Exit(EXIT_FAILURE);
    }
}

__attribute__((noreturn, used)) void Entry(const unsigned long* stack)
{
    if (stack[0] != 4)
    {
        Exit(EXIT_USAGE);
    }

    const char* operation = (const char*)stack[2];
    const char* abi = (const char*)stack[3];
    const char* portText = (const char*)stack[4];
    const int bindSocket = Equals(operation, "bind");
    const int legacy = Equals(abi, "socketcall");
    if ((!bindSocket && !Equals(operation, "listen")) || (!legacy && !Equals(abi, "direct")) || !*portText)
    {
        Exit(EXIT_USAGE);
    }

    unsigned int port = 0;
    while (*portText)
    {
        if (*portText < '0' || *portText > '9' || port > MAX_PORT / 10)
        {
            Exit(EXIT_USAGE);
        }
        port = port * 10 + *portText++ - '0';
    }
    if (port > MAX_PORT || (bindSocket ? port == 0 : port != 0))
    {
        Exit(EXIT_USAGE);
    }

    int descriptor = (int)Socketcall(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    struct SocketAddress address = {AF_INET, (unsigned short)((port >> 8) | (port << 8)), INADDR_ANY, {0}};
    if (bindSocket)
    {
        Check(
            legacy ? Socketcall(SYS_BIND, descriptor, (long)&address, sizeof(address))
                   : Syscall(I386_NR_bind, descriptor, (long)&address, sizeof(address)));
    }
    Check(legacy ? Socketcall(SYS_LISTEN, descriptor, LISTEN_BACKLOG, 0) : Syscall(I386_NR_listen, descriptor, LISTEN_BACKLOG, 0));

    unsigned int addressLength = sizeof(address);
    Socketcall(SYS_GETSOCKNAME, descriptor, (long)&address, (long)&addressLength);
    Write(&address.port, sizeof(address.port));
    WaitForInput(descriptor);
    Socketcall(SYS_ACCEPT, descriptor, 0, 0);
    Exit(EXIT_SUCCESS);
}