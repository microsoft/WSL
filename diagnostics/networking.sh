#! /bin/bash
if (($(id -u) != 0))
then
    printf "If you see 'ping: socket: Operation not permitted' errors, "
    printf "run this command as root.\n"
fi

set -xu

# Gather distro & kernel info.
lsb_release -a || cat /etc/issue /etc/os-release
uname -a

# Output adapter & routing configuration.
ip a
ip route show

# Validate that the gateway is responsive and can route ICMP correctly.
if gateway=$(ip route show | awk '/default/ { print $3 }'); then
    ping -c 4 "$gateway"
else
    echo 'No gateway found.'
fi

ping -c 4 1.1.1.1

# Validate that the default route is working (won't work if traceroute isn't installed).
traceroute 1.1.1.1

# Display the DNS configuration.
cat /etc/resolv.conf

# Validate that everything is functionning correctly.
if type curl >/dev/null 2>&1; then
    curl -m 5 -v https://microsoft.com
else
    wget -T 5 -v https://microsoft.com
fi
