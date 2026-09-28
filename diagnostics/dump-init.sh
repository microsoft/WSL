#! /bin/bash

set -ue

if [ $(whoami) != "root" ]; then
  echo "This script must be run as root in the debug shell (via wsl.exe -u root --debug-shell)"
  exit 1
fi

dmesg

# Try to install gdb

if [ "${dump:-0}" == 1 ]; then
    tdnf install -y gdb || true
fi

declare -a pids_to_dump

for proc in /proc/[0-9]*; do
  read -a stats < "$proc/stat" # Skip kernel threads to make the output easier to read
  flags=${stats[8]}

  if (( ("$flags" & 0x00200000) == 0x00200000 )); then
    continue
  fi

  pid=$(basename "$proc")

  if [ "${dump:-0}" == 1 ]; then
    pids_to_dump+=("$pid")
  fi

  parent=$(ps -o ppid= -p "$pid")

  echo -e "\nProcess: $pid (parent: $parent) "
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

for pid in "${pids_to_dump[@]}" ; do
   name=$(ps -p "$pid" -o comm=)
   if [[ "$name" =~ ^(bash|login)$ ]]; then
     echo "Skipping dump for process: $name"
     continue
   fi

   echo "Dumping process: $name ($pid) "
   if gcore -a -o core "$pid" ; then
     if ! /wsl-capture-crash 0 "$name" "$pid" 0 < "core.$pid" ; then
         echo "Failed to dump process $pid"
     fi

     rm "core.$pid"
   fi
done

echo "hvsockets: "
ss -lap --vsock

echo "meminfo: "
cat /proc/meminfo
