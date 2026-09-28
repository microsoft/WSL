# Localhost

`localhost` is a WSL2 Linux process, created by [mini_init](mini_init.md). Its role is to forward network traffic between the WSL2 virtual machine, and Windows.


## NAT networking 

When `wsl2.networkingMode` is set to NAT, `localhost` will watch for bound TCP ports, and relay the network traffic to Windows via [wslrelay.exe](wslrelay.exe.md)

## Mirrored networking

In mirrored mode, `localhost` registers a BPF program to intercept calls to `bind()`, and forward the calls to Windows via [wslservice.exe](wslservice.exe.md) so Windows can route the network traffic directly to the WSL2 virtual machine.

## Consomme networking

When `wsl2.networkingMode` is set to `Consomme`, the VM is given a virtio-net adapter whose host side is backed by [Consomme][consomme], the user-mode NAT from the [OpenVMM][openvmm] project. The guest sees a normal network (a DHCP-assigned address, a default gateway, and working DNS), but rather than going through a host NAT or bridge, its raw Ethernet frames are parsed by a user-mode process on the host and translated into ordinary host sockets. Because all traffic flows through standard host sockets, host networking policies (firewall, VPN routing, proxy settings) apply just as they would for any other host application.

Bound guest ports are tracked and forwarded to the host, optionally via [wslrelay.exe](wslrelay.exe.md).

See `src/windows/common/ConsommeNetworking.cpp`.

See `src/linux/init/localhost.cpp`.

[consomme]: https://github.com/microsoft/openvmm/tree/main/vm/devices/net/net_consomme/consomme
[openvmm]: https://github.com/microsoft/openvmm