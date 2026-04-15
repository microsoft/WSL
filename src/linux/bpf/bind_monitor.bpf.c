// SPDX-License-Identifier: GPL-2.0

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_endian.h>
#include "bind_monitor.h"

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, BIND_MONITOR_RINGBUF_SIZE);
} events SEC(".maps");

static __always_inline int emit_bind_event(struct sock *sk, int ret)
{
    struct bind_event *e;

    if (!sk || ret != 0)
        return 0;

    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_INET && family != AF_INET6)
        return 0;

    __u16 protocol = BPF_CORE_READ(sk, sk_protocol);
    if (protocol != IPPROTO_TCP && protocol != IPPROTO_UDP)
        return 0;

    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return 0;

    e->family = family;
    e->protocol = protocol;
    e->port = BPF_CORE_READ(sk, __sk_common.skc_num);
    e->pad = 0;

    if (family == AF_INET)
    {
        struct inet_sock *inet = (struct inet_sock *)sk;
        e->addr4 = BPF_CORE_READ(inet, inet_saddr);
        __builtin_memset(e->addr6, 0, sizeof(e->addr6));
    }
    else
    {
        struct ipv6_pinfo *pinet6;
        e->addr4 = 0;
        pinet6 = BPF_CORE_READ((struct inet_sock *)sk, pinet6);
        if (pinet6)
            BPF_CORE_READ_INTO(e->addr6, pinet6, saddr.in6_u.u6_addr8);
        else
            __builtin_memset(e->addr6, 0, sizeof(e->addr6));
    }

    bpf_ringbuf_submit(e, 0);
    return 0;
}

// fexit/inet_bind: called after inet_bind() returns.
SEC("fexit/inet_bind")
int BPF_PROG(fexit_inet_bind, struct socket *sock, struct sockaddr *uaddr,
             int addr_len, int ret)
{
    struct sock *sk = BPF_CORE_READ(sock, sk);
    return emit_bind_event(sk, ret);
}

// fexit/inet6_bind: called after inet6_bind() returns.
SEC("fexit/inet6_bind")
int BPF_PROG(fexit_inet6_bind, struct socket *sock, struct sockaddr *uaddr,
             int addr_len, int ret)
{
    struct sock *sk = BPF_CORE_READ(sock, sk);
    return emit_bind_event(sk, ret);
}

char LICENSE[] SEC("license") = "GPL";
