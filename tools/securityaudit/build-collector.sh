#!/usr/bin/env bash
# Copyright (C) Microsoft Corporation. All rights reserved.
set -euo pipefail

architecture=${1:?Usage: build-collector.sh x64|arm64 build-directory}
build=$(realpath -m "${2:?A dedicated build directory is required}")
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
case "$architecture" in
    x64) package=ebpf.linux-x86_64; options=() ;;
    arm64)
        package=ebpf.linux-arm64
        options=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
            -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
            "-DCMAKE_CROSSCOMPILING_EMULATOR=qemu-aarch64;-L;/usr/aarch64-linux-gnu")
        ;;
    *) echo "Unsupported architecture: $architecture" >&2; exit 1 ;;
esac
version=$(sed -n "s/.*id=\"$package\" version=\"\\([^\"]*\\)\".*/\\1/p" "$root/packages.config")
[[ -n "$version" ]] || { echo "Missing pinned version for $package" >&2; exit 1; }
dotnet restore "$root/tools/securityaudit/Restore.csproj" --configfile "$root/nuget.config" \
    --packages "$build/packages" "-p:BaseIntermediateOutputPath=$build/restore/" \
    "-p:MdePackageId=$package" "-p:MdePackageVersion=$version"
cmake -S "$root/src/linux/securityaudit" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    "-DMDE_PACKAGE_DIR=$build/packages/$package/$version" "${options[@]}"
cmake --build "$build" --parallel 2
cmake --build "$build" --target audit_payload
