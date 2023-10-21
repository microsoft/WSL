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

set +u
[[ ! -v http_proxy ]] && echo "http_proxy is not set" || echo "http_proxy is set"
[[ ! -v HTTP_PROXY ]] && echo "HTTP_PROXY is not set" || echo "HTTP_PROXY is set"
[[ ! -v https_proxy ]] && echo "https_proxy is not set" || echo "https_proxy is set"
[[ ! -v HTTPS_PROXY ]] && echo "HTTPS_PROXY is not set" || echo "HTTPS_PROXY is set"
[[ ! -v no_proxy ]] && echo "no_proxy is not set" || echo "no_proxy is set"
[[ ! -v NO_PROXY ]] && echo "NO_PROXY is not set" || echo "NO_PROXY is set"
[[ ! -v WSL_PAC_URL ]] && echo "WSL_PAC_URL is not set" || echo "WSL_PAC_URL is set"
set -u

echo "Printing DNS configuration"
cat /etc/resolv.conf
