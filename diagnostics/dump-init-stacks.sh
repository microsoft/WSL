#! /bin/bash

set -ue

for proc in /proc/[0-9]*; do
  pid=$(basename "$proc")

  echo -e "\nProcess: $pid"
  echo -en "cmd: "
  cat "/proc/$pid/cmdline" || true
  echo -e "\nstat: "
  cat "/proc/$pid/stat" || true

  for tid in $(ls "/proc/$pid/task" || true); do
    echo -n "tid: $tid - "
    cat "/proc/$pid/task/$tid/comm" || true
    cat "/proc/$pid/task/$tid/stack" || true
  done

  echo "fds: "
  ls -la "/proc/$pid/fd" || true
done

echo "hvsockets: "
ss -lap --vsock

echo "meminfo: "
cat /proc/meminfo