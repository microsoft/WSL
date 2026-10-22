#!/bin/bash
# Copyright (C) Microsoft Corporation. All rights reserved.
set -euo pipefail

if [[ $# != 2 ]]; then
    echo "Usage: bash build-libelf-static.sh <elfutils_0.192.orig.tar.bz2> <dedicated-build-directory>" >&2
    exit 2
fi
archive="$(realpath "$1")"
expected=616099beae24aba11f9b63d86ca6cc8d566d968b802391334c91df54eab416b4
actual="$(sha256sum "$archive")"
if [[ "${actual%% *}" != "$expected" ]]; then
    echo "Expected the unmodified elfutils 0.192 release archive (SHA256 $expected)." >&2
    exit 1
fi
mkdir -p "$2"
root="$(cd "$2" && pwd)"
source_dir="$root/source/elfutils-0.192"
build_dir="$root/build"
prefix="$root/install"
mkdir -p "$root/source" "$build_dir" "$prefix/lib/pkgconfig" "$prefix/include"
if [[ ! -d "$source_dir" ]]; then
    tar -xjf "$archive" -C "$root/source"
fi
(
    cd "$build_dir"
    "$source_dir/configure" "--prefix=$prefix" "--libdir=$prefix/lib" \
        --disable-debuginfod --disable-libdebuginfod --disable-nls --disable-werror
    make -j 1 -C lib libeu.a
    make -j 1 -C libelf libelf.a
)
cp "$build_dir/libelf/libelf.a" "$build_dir/lib/libeu.a" "$prefix/lib/"
cp "$source_dir/libelf/libelf.h" "$source_dir/libelf/gelf.h" "$source_dir/libelf/nlist.h" "$prefix/include/"
cp "$build_dir/config/libelf.pc" "$prefix/lib/pkgconfig/"
# The shared libelf already links libeu; its static consumer needs the same real implementation.
printf '\nLibs.private: -leu -pthread\n' >>"$prefix/lib/pkgconfig/libelf.pc"
printf '%s\n' "$expected" >"$prefix/lib/elfutils-source.sha256"
echo "Use PKG_CONFIG_PATH=$prefix/lib/pkgconfig when building the static audit payload."
