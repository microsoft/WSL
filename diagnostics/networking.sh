#! /bin/bash

set -xu

# Gather distro & kernel info
lsb_release -a || cat /etc/issue
uname -a

# Output adapter & routing configuration
ip a
ip route show

# Validate that the gateway is responsive and can route ICMP correctly
gateway=$(ip route show | awk '/default/ { print $3 }')
ping -c 4 "$gateway"
ping -c 4 1.1.1.1

# Validate that the default route is working (won't work if traceroute isn't installed)
traceroute 1.1.1.1

# Display the DNS configuration
cat /etc/resolv.conf

# Validate that everything is functionning correctly
curl -m 5000 -v https://microsoft.com

