# Copyright (C) Microsoft Corporation. All rights reserved.
#
# Builds the loopback6 relay tc/BPF object and regenerates the loader header that the WSL build embeds.
# Requires clang/llvm and the BPF headers (libbpf-dev + linux-libc-dev on Debian/Ubuntu) plus python3.
#
#   make            # compile loopback6_relay.bpf.o and regenerate loopback6_relay.skel.h
#   make clean
#
# After running `make`, commit the regenerated loopback6_relay.skel.h. The intermediate loopback6_relay.bpf.o
# is a build artifact (ignored by .gitignore) and is not committed; the bytecode it contains is embedded in skel.h.

CLANG ?= clang
PYTHON ?= python3
ARCH := $(shell uname -m | sed 's/x86_64/x86/;s/aarch64/arm64/')

# clang with -target bpf does not add the multiarch include directory, so <asm/types.h> (shipped by
# linux-libc-dev under /usr/include/<triplet>) is not found. Add it explicitly.
MULTIARCH := $(shell uname -m)-linux-gnu

BPF_CFLAGS := -O2 -g -Wall -Werror -target bpf -D__TARGET_ARCH_$(ARCH) -I/usr/include/$(MULTIARCH)

all: loopback6_relay.skel.h

loopback6_relay.bpf.o: loopback6_relay.bpf.c
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

loopback6_relay.skel.h: loopback6_relay.bpf.o gen_bpf_skel.py
	$(PYTHON) gen_bpf_skel.py $< > $@

clean:
	rm -f loopback6_relay.bpf.o

.PHONY: all clean
