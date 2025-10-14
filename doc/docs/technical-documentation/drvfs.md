# Accessing Windows drives from Linux

WSL offers mountpoints to access Windows drives from Linux. These mountpoints are mounted under `/mnt` by default, and point to the root of Windows drives.

## Elevated vs non-elevated mountpoints 

Within a distribution, WSL separates between Linux processes that have been created from an elevated (as in administrator level) and from a non-elevated (user level) context. 

This is done by having two separate [mount namespaces](https://man7.org/linux/man-pages/man7/mount_namespaces.7.html) within the distribution. One of them offers an elevated access to Windows drives, and the other offers a non-elevated access to Windows drives.

When a Linux process is created, [wslservice.exe](wslservice.exe.md) determines its elevation status, and then tell [init](init.md) to create the process in the appropriate mount namespace.

## Mounting a Windows drive

*Note: This section only applies to WSL2 distributions. *

When a [session leader](session-leader.md) is created, [wslservice.exe](wslservice.exe.md) starts a [plan9](https://9fans.github.io/plan9port/man/man9/intro.html) file server. This file server can be connected to from the WSL2 virtual machine to mount Windows drives. 

When the WSL distribution is created, [wslservice.exe](wslservice.exe.md) uses the `LX_INIT_CONFIGURATION_INFORMATION` message to indicate whether the process that created the distribution is elevated or not. Based on this, [init](init.md) will mount either the elevated, or un-elevated version of the plan9 server.

Later when the first command is created in the namespace that hasn't been mounted yet, (either elevated, or non-elevated), [wslservice.exe](wslservice.exe.md) sends a `LxInitMessageRemountDrvfs` to [init](init.md), which tell `init` to mount the other namespace. 

See: `src/windows/service/exe/WslCoreInstance.cpp` and `src/linux/drvfs.cpp`. 

## Mounting a drive from Linux 

As long as the Windows plan9 server is running, drives can be mounted simply by calling [mount](https://linux.die.net/man/8/mount). For instance mounting the C: drive manually can be done via: 

```
mount -t drvfs C: /tmp/my-mount-point
```

Internally, this is handled by `/usr/sbin/mount.drvfs`, which is a symlink to `/init`. When `/init` starts, it looks at `argv[0]` to determine which entrypoint to run. If `argv[0]` is `mount.drvfs`, then `/init` runs the `mount.drvfs` entrypoint (see `MountDrvfsEntry()` in `src/linux/init/drvfs.cpp`).

Depending on the distribution configuration, `mount.drvfs` will either mount the drive as `drvfs` (WSL1), or  `plan9`, `virtio-plan9` or `virtiofs` (WSL), depending on [.wslconfig](https://learn.microsoft.com/windows/wsl/wsl-config).