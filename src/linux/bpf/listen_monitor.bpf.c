// SPDX-License-Identifier: GPL-2.0

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_endian.h>
#include "listen_monitor.h"

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, LISTEN_MONITOR_RINGBUF_SIZE);
} events SEC(".maps");

// Store listen info at listen() time so release can emit correct values
// even if the kernel has already cleared the socket fields.
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, __u64);                // sock pointer as key
    __type(value, struct listen_event); // snapshot from listen time
    __uint(max_entries, 65536);
} listening_sockets SEC(".maps");

static __always_inline int read_sock_info(struct sock *sk, struct listen_event *info)
{
    info->family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (info->family != AF_INET && info->family != AF_INET6)
        return -1;

    info->port = BPF_CORE_READ(sk, __sk_common.skc_num);

    if (info->family == AF_INET)
    {
        struct inet_sock *inet = (struct inet_sock *)sk;
        info->addr4 = BPF_CORE_READ(inet, inet_saddr);
        __builtin_memset(info->addr6, 0, sizeof(info->addr6));
    }
    else
    {
        struct ipv6_pinfo *pinet6;
        info->addr4 = 0;
        pinet6 = BPF_CORE_READ((struct inet_sock *)sk, pinet6);
        if (pinet6)
            BPF_CORE_READ_INTO(info->addr6, pinet6, saddr.in6_u.u6_addr8);
        else
            __builtin_memset(info->addr6, 0, sizeof(info->addr6));
    }

    return 0;
}

// fexit/inet_listen: called after inet_listen() returns.
// inet_listen handles both IPv4 and IPv6.
// int inet_listen(struct socket *sock, int backlog)
SEC("fexit/inet_listen")
int BPF_PROG(fexit_inet_listen, struct socket *sock, int backlog, int ret)
{
    struct listen_event *e;
    struct listen_event info = {};
    __u64 sk_key;
    struct sock *sk;

    if (ret != 0)
        return 0;

    sk = BPF_CORE_READ(sock, sk);
    if (!sk)
        return 0;

    sk_key = (__u64)sk;

    if (read_sock_info(sk, &info) < 0)
        return 0;

    info.is_listen = 1;

    bpf_map_update_elem(&listening_sockets, &sk_key, &info, BPF_ANY);

    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return 0;

    *e = info;
    bpf_ringbuf_submit(e, 0);
    return 0;
}

// fentry/inet_release: called when a socket is being closed.
// void inet_release(struct socket *sock)
SEC("fentry/inet_release")
int BPF_PROG(fentry_inet_release, struct socket *sock)
{
    struct listen_event *e;
    struct listen_event *info;
    struct sock *sk;
    __u64 sk_key;

    sk = BPF_CORE_READ(sock, sk);
    if (!sk)
        return 0;

    sk_key = (__u64)sk;

    info = bpf_map_lookup_elem(&listening_sockets, &sk_key);
    if (!info)
        return 0;

    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
    {
        bpf_map_delete_elem(&listening_sockets, &sk_key);
        return 0;
    }

    *e = *info;
    e->is_listen = 0;

    bpf_map_delete_elem(&listening_sockets, &sk_key);
    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
