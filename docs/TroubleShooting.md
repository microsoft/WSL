# Trouble Shooting
Common troubleshooting issues and solutions are available on our [MSDN documentation](https://msdn.microsoft.com/en-us/commandline/wsl/troubleshooting). Below includes some tips contributed by the community but may not covered by MSDN document yet.

## Missing Kernel or kernel related
The common error message is `WSL 2 requires an update to its kernel component. For information please visit https://aka.ms/wsl2kernel`.
Check [Ship-WSL-2-Linux-Kernel](https://github.com/microsoft/WSL/wiki/Ship-WSL-2-Linux-Kernel) first

## Cannot access wsl files from Windows
9p provides the service on Linux side to allow windows to access Linux file system. If you can't access WSL by \\wsl$ on Windows, most likely 9p is not start up successfully.

You can check the log with `dmesg |grep 9p`, and here is a success output:

```
[    0.363323] 9p: Installing v9fs 9p2000 file system support
[    0.363336] FS-Cache: Netfs '9p' registered for caching
[    0.398989] 9pnet: Installing 9P2000 support
```

You may check if you are hitting/resolve the same issue with #5307

## Can't start WSL 2 distro and only see 'WSL 2' in output
If your display language is not English, then #5388 is possible the root cause. You can change display language to English, then you are able to see the whole error message.

Output only have 'WSL 2'
```
C:\Users\me>wsl
WSL 2
```

## Please enable the Virtual Machine Platform Windows feature and ensure virtualization is enabled in the BIOS.

1. Check the [Hyper-V system requirements](https://docs.microsoft.com/en-us/windows-server/virtualization/hyper-v/system-requirements-for-hyper-v-on-windows#:~:text=on%20Windows%20Server.-,General%20requirements,the%20processor%20must%20have%20SLAT.)
2. If your Windows with WSL itself is a VM, please enable [nested virtualization](https://docs.microsoft.com/en-us/windows/wsl/wsl2-faq#can-i-run-wsl-2-in-a-virtual-machine) manually. Launch powershell with admin, and

    `Set-VMProcessor -VMName <VMName> -ExposeVirtualizationExtensions $true`

3. There is a good link on [enable virtualization Windows 10](https://mashtips.com/enable-virtualization-windows-10/)