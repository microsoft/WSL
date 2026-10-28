#!/usr/bin/env bash
# Copyright (C) Microsoft Corporation. All rights reserved.
set -euo pipefail

architecture=${1:?Expected x64 or arm64}
source /etc/os-release
[[ "$ID" == ubuntu && "$VERSION_ID" == 24.04 ]] || {
    echo "The CI dependency setup requires Ubuntu 24.04." >&2; exit 1;
}
privilege=()
if (( EUID != 0 )); then privilege=(sudo); fi
packages=(cmake make g++ binutils libelf-dev zlib1g-dev
    libicu74 libssl3t64 libkrb5-3 ca-certificates curl git)
case "$architecture" in
    x64) packages+=(libc++1-18 libc++abi1-18 libunwind-18) ;;
    arm64)
        "${privilege[@]}" dpkg --add-architecture arm64
        # Restrict the host mirror to amd64; ARM64 packages come from Ubuntu ports.
        if [[ -f /etc/apt/sources.list.d/ubuntu.sources ]]; then
            "${privilege[@]}" sed -i '/^Architectures:/d; /^Types:/a Architectures: amd64' /etc/apt/sources.list.d/ubuntu.sources
        fi
        if [[ -f /etc/apt/sources.list ]]; then
            "${privilege[@]}" sed -i 's/^deb http/deb [arch=amd64] http/' /etc/apt/sources.list
        fi
        "${privilege[@]}" tee /etc/apt/sources.list.d/wsl-audit-arm64.sources >/dev/null <<'EOF'
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports
Suites: noble noble-updates noble-security
Components: main universe
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
        packages+=(g++-aarch64-linux-gnu binutils-aarch64-linux-gnu qemu-user
            libelf1t64:arm64 zlib1g:arm64 libc++1-18:arm64 libc++abi1-18:arm64 libunwind-18:arm64
            libc6:arm64 libgcc-s1:arm64 libstdc++6:arm64)
        ;;
    *) echo "Unsupported architecture: $architecture" >&2; exit 1 ;;
esac
"${privilege[@]}" apt-get update
"${privilege[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${packages[@]}"
