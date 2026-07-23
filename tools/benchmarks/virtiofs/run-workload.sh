#!/usr/bin/env bash
# Copyright (C) Microsoft Corporation. All rights reserved.

set -uo pipefail

usage() {
    echo "usage: run-workload.sh <git-clone|source-extract> <expected-queue-count> <share-path>..." >&2
    exit 2
}

[[ $# -ge 3 ]] || usage
workload=$1
shift
expected_queue_count=$1
shift
shares=("$@")

case "$workload" in
    git-clone|source-extract)
        ;;
    *)
        usage
        ;;
esac

[[ "$expected_queue_count" =~ ^[1-9][0-9]*$ ]] || usage

mapfile -t virtiofs_devices < <(find /sys/bus/virtio/drivers/virtiofs -mindepth 1 -maxdepth 1 -type l -name 'virtio*' | sort)
if [[ ${#virtiofs_devices[@]} -ne 1 ]]; then
    echo "Expected exactly one virtiofs device, found ${#virtiofs_devices[@]}" >&2
    exit 4
fi

pci_device=$(dirname "$(readlink -f "${virtiofs_devices[0]}")")
msi_vector_count=$(find "$pci_device/msi_irqs" -mindepth 1 -maxdepth 1 -type f | wc -l)
# Virtio PCI uses one config vector. Virtiofs also has one high-priority queue;
# each remaining vector belongs to one request queue.
actual_queue_count=$((msi_vector_count - 2))
if [[ $actual_queue_count -ne $expected_queue_count ]]; then
    echo "Virtiofs request queue mismatch: expected $expected_queue_count, found $actual_queue_count ($msi_vector_count MSI-X vectors)" >&2
    exit 4
fi
printf 'QUEUES\t%s\t%s\t%s\n' "$expected_queue_count" "$actual_queue_count" "$msi_vector_count"

declare -A roots
device=
for index in "${!shares[@]}"; do
    share=${shares[$index]}
    current_device=$(findmnt --noheadings --output MAJ:MIN --target "$share" | xargs)
    current_root=$(findmnt --noheadings --output FSROOT --target "$share" | xargs)

    if [[ -z "$current_device" || -z "$current_root" ]]; then
        echo "Could not resolve mount topology for $share" >&2
        exit 3
    fi
    if [[ -n "$device" && "$current_device" != "$device" ]]; then
        echo "Shares are not on one aggregate device: $current_device != $device" >&2
        exit 3
    fi
    if [[ -n "${roots[$current_root]+present}" ]]; then
        echo "Shares do not have distinct aggregate roots: $current_root" >&2
        exit 3
    fi

    device=$current_device
    roots[$current_root]=1
    printf 'TOPOLOGY\t%s\t%s\t%s\n' "$index" "$current_device" "$current_root"
done

result_directory=$(mktemp -d)
trap 'rm -rf "$result_directory"' EXIT
pids=()
batch_start=$(date +%s%N)

for index in "${!shares[@]}"; do
    share=${shares[$index]}
    (
        worker_start=$(date +%s%N)
        status=0
        case "$workload" in
            git-clone)
                rm -rf "$share/repository"
                git clone --quiet --no-local file:///fixtures/git/repository.git "$share/repository" \
                    >"$result_directory/worker-$index.log" 2>&1 || status=$?
                ;;
            source-extract)
                rm -rf "$share/typescript"
                mkdir -p "$share/typescript"
                tar --extract --file=/fixtures/typescript-source.tar --directory="$share/typescript" \
                    >"$result_directory/worker-$index.log" 2>&1 || status=$?
                ;;
        esac
        worker_end=$(date +%s%N)
        printf 'RESULT\t%s\t%s\t%s\n' "$index" "$(((worker_end - worker_start) / 1000000))" "$status" \
            >"$result_directory/worker-$index.result"
    ) &
    pids+=("$!")
done

for pid in "${pids[@]}"; do
    wait "$pid"
done

batch_end=$(date +%s%N)
printf 'BATCH\t%s\n' "$(((batch_end - batch_start) / 1000000))"

failed=0
for index in "${!shares[@]}"; do
    cat "$result_directory/worker-$index.result"
    status=$(cut -f4 "$result_directory/worker-$index.result")
    if [[ "$status" != "0" ]]; then
        failed=1
        echo "Worker $index failed:" >&2
        cat "$result_directory/worker-$index.log" >&2
    fi
done

exit "$failed"