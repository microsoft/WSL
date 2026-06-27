// Copyright (C) Microsoft Corporation. All rights reserved.
//
// loopback6_relay - tc/clsact eBPF program that makes the IPv6 (::1) localhost relay work end-to-end on
// the GELNIC (loopback0) in Consomme / mirrored networking modes, for servers bound to :: / [::] AND to
// [::1], without breaking guest-originated localhost traffic.
//
// Why this is needed:
//   The host localhost relay (Consomme) injects guest-bound packets onto loopback0 with source ::1 and a
//   destination of the loopback0 global address ("X", dynamically assigned). The stock kernel drops any
//   packet whose source OR destination is the loopback address (::1) when it arrives on a non-loopback
//   device (RFC 4291 2.5.3, enforced in ip6_rcv_core(), incrementing Ip6InHdrErrors). IPv4 avoids this via
//   loopback0's accept_local/route_localnet sysctls; IPv6 has no equivalent and no sysctl disables it.
//   tc/clsact ingress runs *before* ip6_rcv_core(), so it is the earliest place to intervene.
//
// Approach:
//   ingress (loopback0): for a relay-injected packet (source ::1):
//     1. learn X by stashing the destination address into a one-entry map (X is the same for every flow,
//        so a single slot suffices and avoids any per-flow conntrack state);
//     2. rewrite the source ::1 -> SENTINEL (a non-loopback address) so the delivered packet carries a
//        marker the egress hook can recognise;
//     3. rewrite the destination -> ::1 so a socket bound to ::1 (or ::) matches;
//     4. rewrite the destination MAC to lo's address (all zeroes) so that, once redirected, lo classifies
//        the packet PACKET_HOST (a non-matching MAC would be dropped as PACKET_OTHERHOST before ip6_rcv());
//     5. redirect the packet onto lo (ifindex 1). lo has IFF_LOOPBACK, so ip6_rcv_core() does not drop the
//        ::1 destination, and the packet is delivered locally.
//   egress (loopback0): the guest's reply is destined to SENTINEL (because the delivered packet's source
//     was SENTINEL); that destination uniquely identifies a relay reply (guest-originated ::1<->::1 traffic
//     never carries SENTINEL and is left untouched). Rewrite the source ::1 -> X (learned) and the
//     destination SENTINEL -> ::1 so the relay sees the (src=X, dst=::1) reply it expects.
//
// Caveat: a delivered socket sees the peer as SENTINEL (fd00::1) rather than ::1. This is invisible to the
// relay and to typical clients/servers.

#include <linux/bpf.h>
#include <linux/pkt_cls.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/icmpv6.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#define ETH_HLEN_ 14
#define IP6_HLEN_ 40
#define L4_OFF (ETH_HLEN_ + IP6_HLEN_)
#define SADDR_OFF (ETH_HLEN_ + offsetof(struct ipv6hdr, saddr))
#define DADDR_OFF (ETH_HLEN_ + offsetof(struct ipv6hdr, daddr))
#define NEXTHDR_OFF (ETH_HLEN_ + offsetof(struct ipv6hdr, nexthdr))

// lo is always interface index 1 in every network namespace (the kernel creates it first).
#define LO_IFINDEX 1

// One-entry map holding the learned loopback0 global address (X). Legacy definition (no BTF required), so
// the in-tree loader can create it and relocate references without libbpf. Address constants are built on
// the stack (below) rather than stored in .rodata, so this map is the program's only map.
// N.B. struct bpf_map_def is provided by <bpf/bpf_helpers.h>.
struct bpf_map_def SEC("maps") loopback6_xaddr = {
    .type = BPF_MAP_TYPE_ARRAY,
    .key_size = sizeof(__u32),
    .value_size = 16,
    .max_entries = 1,
};

// ::1 (IPv6 loopback) is all zeroes except the last byte == 1.
static __always_inline int is_addr_loopback(const __u8 a[16])
{
#pragma unroll
    for (int i = 0; i < 15; i++)
    {
        if (a[i] != 0)
        {
            return 0;
        }
    }
    return a[15] == 1;
}

// The non-loopback sentinel fd00::1 marks relayed flows.
static __always_inline int is_addr_sentinel(const __u8 a[16])
{
    if (a[0] != 0xfd)
    {
        return 0;
    }
#pragma unroll
    for (int i = 1; i < 15; i++)
    {
        if (a[i] != 0)
        {
            return 0;
        }
    }
    return a[15] == 1;
}

static __always_inline void make_loopback(__u8 a[16])
{
#pragma unroll
    for (int i = 0; i < 16; i++)
    {
        a[i] = 0;
    }
    a[15] = 1;
}

static __always_inline void make_sentinel(__u8 a[16])
{
#pragma unroll
    for (int i = 0; i < 16; i++)
    {
        a[i] = 0;
    }
    a[0] = 0xfd;
    a[15] = 1;
}

// Offset of the L4 checksum field (which covers the IPv6 pseudo-header, hence depends on the addresses).
// Returns -1 for packets we do not rewrite (IPv6 extension headers / unsupported L4 protocols).
static __always_inline int l4_csum_off(__u8 nexthdr)
{
    if (nexthdr == IPPROTO_TCP)
    {
        return L4_OFF + offsetof(struct tcphdr, check);
    }
    if (nexthdr == IPPROTO_UDP)
    {
        return L4_OFF + offsetof(struct udphdr, check);
    }
    if (nexthdr == IPPROTO_ICMPV6)
    {
        return L4_OFF + offsetof(struct icmp6hdr, icmp6_cksum);
    }
    return -1;
}

// Replace the 16-byte IPv6 address at addr_off with new_addr, fixing the L4 (pseudo-header) checksum.
static __always_inline void swap_addr(struct __sk_buff* skb, __u32 addr_off, const __u8* new_addr)
{
    __u8 nexthdr = 0;
    if (bpf_skb_load_bytes(skb, NEXTHDR_OFF, &nexthdr, 1) < 0)
    {
        return;
    }

    int csum_off = l4_csum_off(nexthdr);
    if (csum_off < 0)
    {
        return;
    }

    __u32 oldw[4];
    if (bpf_skb_load_bytes(skb, addr_off, oldw, sizeof(oldw)) < 0)
    {
        return;
    }

    __u32 neww[4];
    __builtin_memcpy(neww, new_addr, sizeof(neww));

#pragma unroll
    for (int i = 0; i < 4; i++)
    {
        if (oldw[i] == neww[i])
        {
            continue;
        }
        bpf_l4_csum_replace(skb, csum_off, oldw[i], neww[i], BPF_F_PSEUDO_HDR | sizeof(__u32));
        bpf_skb_store_bytes(skb, addr_off + i * 4, &neww[i], sizeof(neww[i]), 0);
    }
}

static __always_inline int is_ipv6(struct __sk_buff* skb)
{
    return skb->protocol == bpf_htons(ETH_P_IPV6);
}

// Ingress on loopback0: learn X, mark the source with the sentinel, set the destination to ::1, and
// redirect onto lo so the ::1 destination is accepted and delivered locally.
SEC("tc/ingress")
int loopback6_ingress(struct __sk_buff* skb)
{
    if (!is_ipv6(skb))
    {
        return TC_ACT_OK;
    }

    __u8 saddr[16];
    if (bpf_skb_load_bytes(skb, SADDR_OFF, saddr, sizeof(saddr)) < 0)
    {
        return TC_ACT_OK;
    }

    if (!is_addr_loopback(saddr))
    {
        return TC_ACT_OK;
    }

    // Learn X (the loopback0 global address the relay injected as the destination) for the egress rewrite.
    __u8 daddr[16];
    if (bpf_skb_load_bytes(skb, DADDR_OFF, daddr, sizeof(daddr)) == 0)
    {
        __u32 key = 0;
        __u8* slot = bpf_map_lookup_elem(&loopback6_xaddr, &key);
        if (slot)
        {
            __builtin_memcpy(slot, daddr, 16);
        }
    }

    __u8 sentinel[16];
    make_sentinel(sentinel);
    __u8 loopback[16];
    make_loopback(loopback);

    swap_addr(skb, SADDR_OFF, sentinel);
    swap_addr(skb, DADDR_OFF, loopback);

    // Rewrite the destination MAC to lo's address (all zeroes). The relay injects the packet with
    // loopback0's MAC as the destination; redirecting to lo re-runs eth_type_trans against lo's address, so
    // a non-matching destination MAC would be classified PACKET_OTHERHOST and silently dropped before
    // ip6_rcv() (the packet is still visible to tcpdump's tap, but never delivered). Zeroing the destination
    // MAC matches lo's dev_addr, so the packet is classified PACKET_HOST and delivered locally.
    __u8 lomac[6] = {0, 0, 0, 0, 0, 0};
    bpf_skb_store_bytes(skb, 0, lomac, sizeof(lomac), 0);

    return bpf_redirect(LO_IFINDEX, BPF_F_INGRESS);
}

// Egress on loopback0: a destination of SENTINEL uniquely identifies a relay reply. Restore the source to
// the learned X and the destination to ::1 so the relay matches the flow.
SEC("tc/egress")
int loopback6_egress(struct __sk_buff* skb)
{
    if (!is_ipv6(skb))
    {
        return TC_ACT_OK;
    }

    __u8 daddr[16];
    if (bpf_skb_load_bytes(skb, DADDR_OFF, daddr, sizeof(daddr)) < 0)
    {
        return TC_ACT_OK;
    }

    if (!is_addr_sentinel(daddr))
    {
        return TC_ACT_OK;
    }

    __u32 key = 0;
    __u8* x = bpf_map_lookup_elem(&loopback6_xaddr, &key);
    if (x)
    {
        swap_addr(skb, SADDR_OFF, x);
    }

    __u8 loopback[16];
    make_loopback(loopback);
    swap_addr(skb, DADDR_OFF, loopback);
    return TC_ACT_OK;
}

char _license[] SEC("license") = "Dual BSD/GPL";
