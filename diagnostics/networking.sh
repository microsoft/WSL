#! /bin/bash
set -xu

# Gather distro & kernel info.
lsb_release -a || cat /etc/issue /etc/os-release
uname -a

echo "Printing per-distro configuration"
cat /etc/wsl.conf

echo "Printing adapter & routing configuration"
ip a
ip route show table all
ip neighbor
ip link
ip rule show

# We only print whether the HTTP proxy variables are set or not, as their content might contain secrets such as usernames, passwords.
if [ -z ${http_proxy+x} ]; then echo "http_proxy is unset"; else echo "http_proxy is set"; fi
if [ -z ${HTTP_PROXY+x} ]; then echo "HTTP_PROXY is unset"; else echo "HTTP_PROXY is set"; fi
if [ -z ${https_proxy+x} ]; then echo "https_proxy is unset"; else echo "https_proxy is set"; fi
if [ -z ${HTTPS_PROXY+x} ]; then echo "HTTPS_PROXY is unset"; else echo "HTTPS_PROXY is set"; fi
if [ -z ${no_proxy+x} ]; then echo "no_proxy is unset"; else echo "no_proxy is set"; fi
if [ -z ${NO_PROXY+x} ]; then echo "NO_PROXY is unset"; else echo "NO_PROXY is set"; fi
if [ -z ${WSL_PAC_URL+x} ]; then echo "WSL_PAC_URL is unset"; else echo "WSL_PAC_URL is set"; fi

echo "Printing DNS configuration"
cat /etc/resolv.conf
cat /etc/nsswitch.conf

echo "Printing iptables and nftables rules"
# iptables can be configured using both "iptables" and the legacy version "iptables-legacy". It's possible they can be used together
# (although not recommended). Collect both to make sure no rules are missed.
# We list the contents of the most common tables (filter, nat, mangle, raw, security)
iptables -vL -t filter
iptables -vL -t nat
iptables -vL -t mangle
iptables -vL -t raw
iptables -vL -t security

ip6tables -vL -t filter
ip6tables -vL -t nat
ip6tables -vL -t mangle
ip6tables -vL -t raw
ip6tables -vL -t security

iptables-legacy -vL -t filter
iptables-legacy -vL -t nat
iptables-legacy -vL -t mangle
iptables-legacy -vL -t raw
iptables-legacy -vL -t security

ip6tables-legacy -vL -t filter
ip6tables-legacy -vL -t nat
ip6tables-legacy -vL -t mangle
ip6tables-legacy -vL -t raw
ip6tables-legacy -vL -t security

nft list ruleset

# This will print configurations such as net.ipv4.conf.all.route_localnet or net.ipv4.ip_local_port_range
echo "Printing networking configurations"
sysctl -a | grep net
