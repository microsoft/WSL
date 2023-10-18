#! /bin/bash
set -xu

# Gather distro & kernel info.
lsb_release -a || cat /etc/issue /etc/os-release
uname -a

echo "Printing adapter & routing configuration"
ip a
ip route show table all
ip neighbor
ip link

# This will include the HTTP proxy env variables, if present
echo "Printing all environment variables"
printenv

echo "Printing DNS configuration"
cat /etc/resolv.conf
