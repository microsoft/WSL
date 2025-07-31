# Localhost

`localhost` is a WSL2 linux process, created by [mini_init](mini_init.md). Its role is to forward network traffic between the WSL2 virtual machine, and Windows.


## NAT networking 

When `wsl2.networkingMode` is set to NAT, `localhost` will watch for bound TCP ports, and relay the network traffic to Windows via [wslrelay.exe](wslrelay.exe.md)

## Mirrored networking

In mirrored mode, `localhost` registers a BPF program to intercept calls to `bind()`, and forward the calls to Windows via [wslservice.exe](wslservice.exe.md) so Windows can route the network traffic directly to the WSL2 virtual machine.

See `src/linux/localhost.cpp`.