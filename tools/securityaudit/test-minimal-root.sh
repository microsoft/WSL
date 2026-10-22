#!/bin/bash
# Copyright (C) Microsoft Corporation. All rights reserved.
set -euo pipefail

if [[ $# != 3 ]]; then
    echo "Usage: bash test-minimal-root.sh <live-test-executable> <payload-directory> <new-root-directory>" >&2
    exit 2
fi
test_executable="$(realpath "$1")"
payload="$(realpath "$2")"
root="$(realpath -m "$3")"
mkdir "$root"
trap 'rm -rf -- "$root"' EXIT
mkdir "$root/proc" "$root/sys" "$root/run"
cp "$payload/wsl-audit-collector" "$root/"
cp -a "$payload/wsl-security-audit" "$root/"
cp "$payload/audit-payload.json" "$root/wsl-security-audit/provenance.json"
# Leave proc/sys empty: the production collector must mount its kernel filesystems privately.
# No libc, interpreter, runtime tree or external plugin mount is supplied.
unshare --mount --propagation private /bin/bash -c '
    set -e
    root="$1"
    mount --bind "$root" "$root"
    exec "$2" "$3" "$4" --minimal-root "$root"
' audit-minimal-root "$root" "$test_executable" "$payload/wsl-audit-collector" "$payload/wsl-security-audit"
