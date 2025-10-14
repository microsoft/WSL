# GNS

`gns` is a process created by `mini_init`. Its job is to configure networking within the WSL2 virtual machine. 

## Networking configuration 

Networking settings are shared by all WSL2 distributions. While WSL2 is running, `gns` maintains an hvsocket channel to [wslservice.exe](wslservice.exe.md), which is used to send various networking related configurations such as:

- Interface IP configuration
- Routing table entries
- DNS configuration
- MTU size configuration

When DNS tunneling is enabled, `gns` is also responsible for replying to DNS requests.

See `src/linux/init/GnsEngine.cpp` and `src/windows/service/exe/GnsChannel.cpp`