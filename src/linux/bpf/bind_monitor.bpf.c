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

// Track sockets that went through bind() so we only emit matching releases.
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, __u64);    // sock pointer as key
    __type(value, __u8);
    __uint(max_entries, 65536);
} bound_sockets SEC(".maps");

static __always_inline int emit_event(struct sock *sk, __u8 is_bind, int ret)
{
    struct bind_event *e;
    __u16 family;
    __u16 protocol;
    __u64 sk_key = (__u64)sk;

    if (!sk)
        return 0;

    family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_INET && family != AF_INET6)
        return 0;

    protocol = BPF_CORE_READ(sk, sk_protocol);
    if (protocol != IPPROTO_TCP && protocol != IPPROTO_UDP)
        return 0;

    if (is_bind)
    {
        // Only emit successful binds.
        if (ret != 0)
            return 0;

        __u8 val = 1;
        bpf_map_update_elem(&bound_sockets, &sk_key, &val, BPF_ANY);
    }
    else
    {
        // Only emit release if this socket was previously bound.
        if (!bpf_map_lookup_elem(&bound_sockets, &sk_key))
            return 0;
        bpf_map_delete_elem(&bound_sockets, &sk_key);
    }

    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return 0;

    e->family = family;
    e->protocol = protocol;
    e->port = bpf_ntohs(BPF_CORE_READ(sk, __sk_common.skc_num));
    e->is_bind = is_bind;
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
// int inet_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len)
SEC("fexit/inet_bind")
int BPF_PROG(fexit_inet_bind, struct socket *sock, struct sockaddr *uaddr,
             int addr_len, int ret)
{
    struct sock *sk = BPF_CORE_READ(sock, sk);
    return emit_event(sk, 1, ret);
}

// fexit/inet6_bind: called after inet6_bind() returns.
// int inet6_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len)
SEC("fexit/inet6_bind")
int BPF_PROG(fexit_inet6_bind, struct socket *sock, struct sockaddr *uaddr,
             int addr_len, int ret)
{
    struct sock *sk = BPF_CORE_READ(sock, sk);
    return emit_event(sk, 1, ret);
}

// fentry/inet_release: called when a socket is being closed.
// void inet_release(struct socket *sock)
SEC("fentry/inet_release")
int BPF_PROG(fentry_inet_release, struct socket *sock)
{
    struct sock *sk = BPF_CORE_READ(sock, sk);
    return emit_event(sk, 0, 0);
}

char LICENSE[] SEC("license") = "GPL";
