#! /bin/bash
set -xu

# Gather distro & kernel info.
lsb_release -a || cat /etc/issue /etc/os-release
uname -a

# Output adapter & routing configuration.
ip a
ip route show table all
ip neighbor
ip link

# Display the DNS configuration.
cat /etc/resolv.conf
