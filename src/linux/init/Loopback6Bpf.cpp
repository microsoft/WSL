// Copyright (C) Microsoft Corporation. All rights reserved.
//
// Minimal in-tree loader for the loopback6 relay tc/BPF programs. The program bytecode and the byte
// offsets of its map references are embedded by loopback6_relay.skel.h (generated from the compiled
// object by src/linux/bpf/gen_bpf_skel.py). This avoids a libbpf dependency in init: it creates the
// single (legacy, BTF-less) array map, patches the map fd into the ld_imm64 instructions that reference
// it, and loads each SCHED_CLS program via the bpf() syscall.

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/bpf.h>
#include "common.h"
#include "Loopback6Bpf.h"
#include "loopback6_relay.skel.h"

namespace {

int SysBpf(int cmd, union bpf_attr* attr)
{
    return static_cast<int>(syscall(__NR_bpf, cmd, attr, sizeof(*attr)));
}

int CreateMap()
{
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_type = LOOPBACK6_MAP_TYPE;
    attr.key_size = LOOPBACK6_MAP_KEY_SIZE;
    attr.value_size = LOOPBACK6_MAP_VALUE_SIZE;
    attr.max_entries = LOOPBACK6_MAP_MAX_ENTRIES;
    attr.map_flags = LOOPBACK6_MAP_FLAGS;
    return SysBpf(BPF_MAP_CREATE, &attr);
}

int LoadProgram(const unsigned char* insnBytes, unsigned int insnByteLen, const unsigned int* relocs, unsigned int relocCount, int mapFd)
{
    std::vector<uint8_t> insns(insnBytes, insnBytes + insnByteLen);

    // Patch the map fd into each ld_imm64 instruction that references the map.
    for (unsigned int i = 0; i < relocCount; i++)
    {
        if (relocs[i] + sizeof(struct bpf_insn) > insns.size())
        {
            continue;
        }
        auto* insn = reinterpret_cast<struct bpf_insn*>(insns.data() + relocs[i]);
        insn->src_reg = BPF_PSEUDO_MAP_FD;
        insn->imm = mapFd;
    }

    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.prog_type = BPF_PROG_TYPE_SCHED_CLS;
    attr.insn_cnt = insnByteLen / static_cast<unsigned int>(sizeof(struct bpf_insn));
    attr.insns = reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(insns.data()));
    attr.license = reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(LOOPBACK6_LICENSE));

    // First attempt the load without a verifier log. Requesting a verbose log (log_level >= 1) with a
    // fixed-size buffer makes BPF_PROG_LOAD fail with ENOSPC when the per-instruction log exceeds the
    // buffer, even though the program is valid. Only on failure do we retry with a log to capture the
    // verifier diagnostics.
    int fd = SysBpf(BPF_PROG_LOAD, &attr);
    if (fd < 0)
    {
        char log[8192];
        log[0] = '\0';
        attr.log_level = 1;
        attr.log_buf = reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(log));
        attr.log_size = sizeof(log);

        fd = SysBpf(BPF_PROG_LOAD, &attr);
        if (fd < 0)
        {
            GNS_LOG_ERROR("BPF_PROG_LOAD failed (errno {}); verifier log: {}", errno, log);
        }
    }
    return fd;
}

} // namespace

std::optional<Loopback6BpfPrograms> LoadLoopback6RelayPrograms()
{
    if (LOOPBACK6_INGRESS_INSN_BYTES == 0 || LOOPBACK6_EGRESS_INSN_BYTES == 0)
    {
        GNS_LOG_INFO("loopback6 relay BPF programs are not built into this image; skipping inbound ::1 setup");
        return std::nullopt;
    }

    int mapFd = CreateMap();
    if (mapFd < 0)
    {
        GNS_LOG_ERROR("BPF_MAP_CREATE failed (errno {})", errno);
        return std::nullopt;
    }

    int ingressFd =
        LoadProgram(kLoopback6IngressInsns, LOOPBACK6_INGRESS_INSN_BYTES, kLoopback6IngressMapRelocs, LOOPBACK6_INGRESS_MAP_RELOC_COUNT, mapFd);
    int egressFd =
        LoadProgram(kLoopback6EgressInsns, LOOPBACK6_EGRESS_INSN_BYTES, kLoopback6EgressMapRelocs, LOOPBACK6_EGRESS_MAP_RELOC_COUNT, mapFd);

    // The loaded programs hold their own reference to the map; the loader's fd is no longer needed.
    close(mapFd);

    if (ingressFd < 0 || egressFd < 0)
    {
        if (ingressFd >= 0)
        {
            close(ingressFd);
        }
        if (egressFd >= 0)
        {
            close(egressFd);
        }
        return std::nullopt;
    }

    return Loopback6BpfPrograms{ingressFd, egressFd};
}
