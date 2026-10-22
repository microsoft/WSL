#!/bin/bash
# Copyright (C) Microsoft Corporation. All rights reserved.
set -euo pipefail

if [[ $# != 4 ]]; then
    echo "Usage: bash build-payload.sh <ebpf-source> <json-include> <build-directory> <payload-directory>" >&2
    exit 2
fi
source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ebpf_root="$(cd "$1" && pwd)"
json_include="$(cd "$2" && pwd)"
mkdir -p "$3" "$4"
build_dir="$(cd "$3" && pwd)"
payload_dir="$(cd "$4" && pwd)"
architecture="$(uname -m)"
case "$architecture" in
    x86_64|aarch64) ;;
    *) echo "Only native x86_64 and aarch64 Linux builders are supported." >&2; exit 1 ;;
esac
if [[ -f "$build_dir/audit-build-architecture" && "$(cat "$build_dir/audit-build-architecture")" != "$architecture" ]]; then
    echo "Build directory belongs to another architecture; choose a separate build directory." >&2
    exit 1
fi
printf '%s\n' "$architecture" >"$build_dir/audit-build-architecture"
for tool in cmake make clang clang++ readelf git pkg-config; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done
if [[ ! -f "$ebpf_root/libbpf/src/Makefile" ]]; then
    echo "Missing pinned libbpf submodule. Initialize it in the authorized eBPF source checkout before building." >&2
    exit 1
fi
(
    cd "$ebpf_root"
    bash ./build.sh -S -B "$build_dir/ebpf" -j 1
)
cmake -S "$source_root/tools/securityaudit" -B "$build_dir" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ \
    "-DWSL_AUDIT_EBPF_SOURCE_ROOT=$ebpf_root" \
    "-DWSL_AUDIT_EBPF_STATIC_PACKAGE=$build_dir/ebpf/package" \
    "-DWSL_AUDIT_JSON_INCLUDE=$json_include" "-DWSL_AUDIT_PAYLOAD_OUTPUT=$payload_dir"
cmake --build "$build_dir" --parallel 1
cmake --build "$build_dir" --target audit_payload --parallel 1
echo "Static payload: $payload_dir"
echo "Offline regression tests (after the coordinated full WSL build): ctest --test-dir \"$build_dir\" --output-on-failure"
echo "Privileged live validation remains required; see tools/securityaudit/README.md."
