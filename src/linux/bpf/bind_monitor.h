// Copyright (C) Microsoft Corporation. All rights reserved.

#pragma once

#ifndef __u8
typedef unsigned char __u8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef unsigned long long __u64;
#endif

#ifndef AF_INET
#define AF_INET   2
#endif
#ifndef AF_INET6
#define AF_INET6  10
#endif

#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP 17
#endif

#define BIND_MONITOR_RINGBUF_SIZE (1 << 16) /* 64 KB */

struct bind_event {
    __u32 family;       /* AF_INET or AF_INET6 */
    __u32 protocol;     /* IPPROTO_TCP or IPPROTO_UDP */
    __u16 port;         /* host byte order */
    __u8  is_bind;      /* 1 = bind, 0 = release */
    __u8  pad;
    __u32 addr4;        /* IPv4 address (network byte order) */
    __u8  addr6[16];    /* IPv6 address */
};
