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

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, __u64);               // sock pointer as key
    __type(value, struct bind_event); // snapshot from bind time
    __uint(max_entries, 65536);
} bound_sockets SEC(".maps");

static __always_inline int emit_bind_event(struct sock *sk, int ret)
{
    struct bind_event *e;
    struct bind_event info = {};
    __u64 sk_key = (__u64)sk;

    if (!sk || ret != 0)
        return 0;

    info.family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (info.family != AF_INET && info.family != AF_INET6)
        return 0;

    info.protocol = BPF_CORE_READ(sk, sk_protocol);
    if (info.protocol != IPPROTO_TCP && info.protocol != IPPROTO_UDP)
        return 0;

    info.port = BPF_CORE_READ(sk, __sk_common.skc_num);
    info.is_bind = 1;

    if (info.family == AF_INET)
    {
        struct inet_sock *inet = (struct inet_sock *)sk;
        info.addr4 = BPF_CORE_READ(inet, inet_saddr);
    }
    else
    {
        struct ipv6_pinfo *pinet6;
        pinet6 = BPF_CORE_READ((struct inet_sock *)sk, pinet6);
        if (pinet6)
            BPF_CORE_READ_INTO(info.addr6, pinet6, saddr.in6_u.u6_addr8);
    }

    bpf_map_update_elem(&bound_sockets, &sk_key, &info, BPF_ANY);

    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return 0;

    *e = info;
    bpf_ringbuf_submit(e, 0);
    return 0;
}

static __always_inline int emit_release_event(struct sock *sk)
{
    struct bind_event *e;
    struct bind_event *info;
    __u64 sk_key = (__u64)sk;

    if (!sk)
        return 0;

    info = bpf_map_lookup_elem(&bound_sockets, &sk_key);
    if (!info)
        return 0;

    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
    {
        bpf_map_delete_elem(&bound_sockets, &sk_key);
        return 0;
    }

    *e = *info;
    e->is_bind = 0;

    bpf_map_delete_elem(&bound_sockets, &sk_key);
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
    return emit_bind_event(sk, ret);
}

// fexit/inet6_bind: called after inet6_bind() returns.
// int inet6_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len)
SEC("fexit/inet6_bind")
int BPF_PROG(fexit_inet6_bind, struct socket *sock, struct sockaddr *uaddr,
             int addr_len, int ret)
{
    struct sock *sk = BPF_CORE_READ(sock, sk);
    return emit_bind_event(sk, ret);
}

// fentry/inet_release: called when a socket is being closed.
// void inet_release(struct socket *sock)
SEC("fentry/inet_release")
int BPF_PROG(fentry_inet_release, struct socket *sock)
{
    struct sock *sk = BPF_CORE_READ(sock, sk);
    return emit_release_event(sk);
}

char LICENSE[] SEC("license") = "GPL";
