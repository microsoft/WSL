# Running Windows executables from Linux

The ability to launch Windows processes from Linux is controlled by 2 different levels of settings: 

- The `HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\LxssManager\DistributionFlags` registry value, which controls the settings for all Windows users (setting the lowest significance bit disables interop)
- The `[interop]` section in [/etc/wsl.conf](https://learn.microsoft.com/windows/wsl/wsl-config#wslconf), which controls the setting for a given WSL distribution.

## binfmt interpreters for Windows executables 

To allow Windows process creation from Linux, WSL registers a [binfmt interpreter](https://docs.kernel.org/admin-guide/binfmt-misc.html), which tells the kernel to execute an arbitrary command when a specific type of executable is launched via `exec*()` system calls.

To perform the registration, WSL writes to `/proc/sys/fs/binfmt_misc` and creates a `WSLInterop` entry, which points to `/init`. For WSL1 registration, the entry is written by [init](init.md) for each distribution, for WSL2 [mini_init](mini_init.md) registers the binfmt interpreter at the virtual machine level. 

With systemd enabled, WSL also restores `WSLInterop` after `systemd-binfmt.service` and `binfmt-support.service` have started. A monitor watches subsequent binfmt registrations and restores its priority when another handler is registered, including during package installation or upgrades. Only one monitor updates the VM-wide registry at a time; another takes over when its distro exits. Since the most recently registered matching handler takes precedence, this keeps distro-provided `MZ` handlers from intercepting Windows executables. Setting `[boot] protectBinfmt=false` in `/etc/wsl.conf` disables this distro's monitor and its protection against VM-wide binfmt removal at shutdown.

Note: The `/init` executable is the entrypoint for different WSL processes ([init](init.md), [plan9](plan9.md), [localhost](localhost.md), etc). This executable looks at `argv[0]` to determine which logic to run. In the case of interop, `/init` will run the Windows process creation logic if its `argv[0]` value doesn't match any of the known entrypoints.

See: `WslEntryPoint()` in `src/linux/init.cpp`.

## Connecting to interop servers

When the user tries to execute a Windows process, the kernel will launch `/init` with the Windows process's command line as arguments. 

To start a new Windows process `/init` needs to connect to an interop server. Interop servers are special Linux processes that act as bridges between Linux and Windows. They maintain secure communication channels (through hvsocket connections) with Windows processes ([wsl.exe](wsl.exe.md) or [wslhost.exe](wslhost.exe.md)) to launch Windows executables.

Inside Linux, each [session leader](session-leader.md), and each instance of [init](init.md) has an associated interop server, which is serving via an unix socket under `/run/WSL`.

`/init` uses the `$WSL_INTEROP` environment variable to know which server to connect to. If the variable is not set, `/init` will try to connect to `/run/WSL/${pid}_interop`, with its own PID. If that doesn't work, `/init` will try its parent's pid, and then will continue to go up the chain until it reached [init](init.md).

Once connected `/init` sends a `LxInitMessageCreateProcess` (WSL1) or a `LxInitMessageCreateProcessUtilityVm` (WSL2), which then forwards that message to the associated Windows process, which will launch the requested command and relay its output to `/init`. 

See `src/linux/init/binfmt.cpp`