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

echo "Printing iptables and nftables rules"
iptables -S
nft list ruleset
