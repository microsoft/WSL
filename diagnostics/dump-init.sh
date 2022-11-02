#! /bin/bash

set -ue

if [ $(whoami) != "root" ]; then
  echo "This script must be run as root in the system distro (via wsl.exe -u root --system)"
  exit 1
fi

ts=$(date +%F_%H-%M-%S)
target="/mnt/c/wsl-init-dump-$ts"

mkdir -p "$target"

tdnf install -y gdb

bash -c "cd $target && gcore -a \$(pgrep init)"

stack_log="$target/stacks.txt"
fd_log="$target/fd.txt"
for pid in $(pgrep init); do
  echo -e "\nProcess: $pid" >> "$stack_log"
  echo -e "\nProcess: $pid" >> "$fd_log"
  for tid in $(ls "/proc/$pid/task" || true); do
    echo "tid: $tid" >> "$stack_log"
    cat "/proc/$pid/task/$tid/stack" >> "$stack_log" || true
  done

  ls -la "/proc/$pid/fd" >> "$fd_log" || true
done


ss -lap --vsock > "/$target/sockets.txt"
dmesg > "/$target/dmesg.txt"
cat /proc/meminfo > "/$target/meminfo.txt"

echo "Logs and dumps written in $target"
