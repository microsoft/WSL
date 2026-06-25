#!/usr/bin/env python3
# Copyright (C) Microsoft Corporation. All rights reserved.
#
# Turns a compiled BPF object (loopback6_relay.bpf.o) into a self-contained C header that the in-tree
# loader (src/linux/init/Loopback6Bpf.cpp) embeds and loads without libbpf.
#
# For each program section ("tc/ingress", "tc/egress") it emits the raw instruction bytes plus the byte
# offsets of the instructions that reference the single map "loopback6_xaddr" (so the loader can patch in
# the map fd). The object must contain exactly one map and no .rodata/.bss/.data maps (the program builds
# its address constants on the stack), so map-fd relocations are the only relocations present.
#
#   ./gen_bpf_skel.py loopback6_relay.bpf.o > loopback6_relay.skel.h

import struct
import sys

MAP_SYMBOL = b"loopback6_xaddr"
PROG_SECTIONS = ("tc/ingress", "tc/egress")

R_BPF_64_64 = 1  # ld_imm64 map reference


def fail(msg):
    sys.stderr.write("gen_bpf_skel.py: " + msg + "\n")
    sys.exit(1)


class Section:
    __slots__ = ("name", "type", "offset", "size", "link", "info", "entsize")


def parse_elf(data):
    if data[:4] != b"\x7fELF":
        fail("not an ELF file")
    if data[4] != 2:
        fail("expected a 64-bit ELF object")
    little = data[5] == 1
    end = "<" if little else ">"

    # ELF64 header: e_shoff at 0x28 (8), e_shentsize at 0x3a (2), e_shnum at 0x3c (2), e_shstrndx at 0x3e (2)
    e_shoff = struct.unpack_from(end + "Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from(end + "H", data, 0x3A)[0]
    e_shnum = struct.unpack_from(end + "H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from(end + "H", data, 0x3E)[0]

    raw = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        sh_name, sh_type = struct.unpack_from(end + "II", data, base)
        sh_offset = struct.unpack_from(end + "Q", data, base + 0x18)[0]
        sh_size = struct.unpack_from(end + "Q", data, base + 0x20)[0]
        sh_link, sh_info = struct.unpack_from(end + "II", data, base + 0x28)
        sh_entsize = struct.unpack_from(end + "Q", data, base + 0x38)[0]
        raw.append((sh_name, sh_type, sh_offset, sh_size, sh_link, sh_info, sh_entsize))

    shstr_off = raw[e_shstrndx][2]
    shstr_size = raw[e_shstrndx][3]
    shstrtab = data[shstr_off:shstr_off + shstr_size]

    def cstr(table, off):
        nul = table.find(b"\x00", off)
        return table[off:nul]

    sections = []
    for (sh_name, sh_type, sh_offset, sh_size, sh_link, sh_info, sh_entsize) in raw:
        s = Section()
        s.name = cstr(shstrtab, sh_name).decode("utf-8", "replace")
        s.type = sh_type
        s.offset = sh_offset
        s.size = sh_size
        s.link = sh_link
        s.info = sh_info
        s.entsize = sh_entsize
        sections.append(s)

    return end, sections


def section_data(data, s):
    return data[s.offset:s.offset + s.size]


def symbol_names(end, data, sections):
    symtab = next((s for s in sections if s.name == ".symtab"), None)
    if symtab is None:
        fail("missing .symtab")
    strtab = sections[symtab.link]
    strtab_data = section_data(data, strtab)
    syms = section_data(data, symtab)
    names = []
    # Elf64_Sym: st_name (4), st_info (1), st_other (1), st_shndx (2), st_value (8), st_size (8) = 24 bytes
    for off in range(0, len(syms), 24):
        st_name = struct.unpack_from(end + "I", syms, off)[0]
        nul = strtab_data.find(b"\x00", st_name)
        names.append(strtab_data[st_name:nul])
    return names


def map_reloc_offsets(end, data, sections, prog_index, sym_names):
    rel_name = ".rel" + sections[prog_index].name
    rel = next((s for s in sections if s.name == rel_name), None)
    if rel is None:
        return []
    rel_data = section_data(data, rel)
    offsets = []
    # Elf64_Rel: r_offset (8), r_info (8); r_sym = r_info >> 32
    for off in range(0, len(rel_data), 16):
        r_offset, r_info = struct.unpack_from(end + "QQ", rel_data, off)
        r_sym = r_info >> 32
        r_type = r_info & 0xFFFFFFFF
        if r_type != R_BPF_64_64:
            continue
        if r_sym < len(sym_names) and sym_names[r_sym] == MAP_SYMBOL:
            offsets.append(r_offset)
    return sorted(offsets)


def c_bytes(name, blob):
    if not blob:
        return "static const unsigned char %s[1] = {0};\n" % name
    out = ["static const unsigned char %s[] = {" % name]
    line = "    "
    for i, b in enumerate(blob):
        line += "0x%02x, " % b
        if (i + 1) % 12 == 0:
            out.append(line)
            line = "    "
    if line.strip():
        out.append(line)
    out.append("};\n")
    return "\n".join(out)


def c_uints(name, values):
    if not values:
        return "static const unsigned int %s[1] = {0};\n" % name
    return "static const unsigned int %s[] = {%s};\n" % (name, ", ".join(str(v) for v in values))


def main():
    if len(sys.argv) != 2:
        fail("usage: gen_bpf_skel.py <object.o>")
    with open(sys.argv[1], "rb") as f:
        data = f.read()

    end, sections = parse_elf(data)
    sym_names = symbol_names(end, data, sections)

    by_name = {s.name: i for i, s in enumerate(sections)}
    for prog in PROG_SECTIONS:
        if prog not in by_name:
            fail("missing program section '%s'" % prog)

    out = []
    out.append("// Copyright (C) Microsoft Corporation. All rights reserved.")
    out.append("//")
    out.append("// Generated by src/linux/bpf/gen_bpf_skel.py from loopback6_relay.bpf.o. Do not edit by hand.")
    out.append("")
    out.append("#pragma once")
    out.append("")
    out.append("#define LOOPBACK6_MAP_TYPE 2 /* BPF_MAP_TYPE_ARRAY */")
    out.append("#define LOOPBACK6_MAP_KEY_SIZE 4")
    out.append("#define LOOPBACK6_MAP_VALUE_SIZE 16")
    out.append("#define LOOPBACK6_MAP_MAX_ENTRIES 1")
    out.append("#define LOOPBACK6_MAP_FLAGS 0")
    out.append('#define LOOPBACK6_LICENSE "Dual BSD/GPL"')
    out.append("")

    labels = (("INGRESS", "Ingress", "tc/ingress"), ("EGRESS", "Egress", "tc/egress"))
    for upper, camel, secname in labels:
        idx = by_name[secname]
        blob = section_data(data, sections[idx])
        if len(blob) % 8 != 0:
            fail("section '%s' size %d is not a multiple of 8" % (secname, len(blob)))
        relocs = map_reloc_offsets(end, data, sections, idx, sym_names)
        out.append("#define LOOPBACK6_%s_INSN_BYTES %d" % (upper, len(blob)))
        out.append("#define LOOPBACK6_%s_MAP_RELOC_COUNT %d" % (upper, len(relocs)))
        out.append(c_bytes("kLoopback6%sInsns" % camel, blob))
        out.append(c_uints("kLoopback6%sMapRelocs" % camel, relocs))
        out.append("")

    sys.stdout.write("\n".join(out))


if __name__ == "__main__":
    main()
