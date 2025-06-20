# Init

Init is top level process of a WSL distribution. For WSL1 distributions, it is launched by [wslservice](wslservice.exe.md) (see `src/windows/service/LxssInstance.cpp`) and for WSL2 distributions, it is launched by [mini_init](mini_init.md).

## WSL2 specific distributions startup

Each WSL2 distributions runs in a separate mount, pid and UTS namespace. This allows distributions to run in parallel, without "seeing" each other. 

When a WSL2 distribution starts, [mini_init](mini_init.md):

- Mounts the distribution VHD
- Clones into a child namespace
- Chroots in the VHD mountpoint
- Executes init (see the `LxMiniInitMessageLaunchInit` message).

While each distribution runs in its own mount namespace, the `/mnt/wsl` mountpoint is shared between all distributions.

## Distribution initialization

Once started, the `init` process performs various initialization tasks such as:

- Mounting `/proc`, `/sys` and `/dev`
- Configuring cgroups
- Registering the binfmt interpreter (see [interop](interop.md))
- Parsing [/etc/wsl.conf](https://learn.microsoft.com/windows/wsl/wsl-config)
- Starting systemd (see [systemd](systemd.md))
- Mounting `drvfs` drives (See [drvfs](drvfs.md))
- Configuring `wslg` (see [wslg](https://github.com/microsoft/wslg))

## Running the distribution

Once ready, `init` establishes either an `lxbus` (WSL1) or an  `hvsocket` (WSL2) connection to [wslservice](wslservice.exe.md). This channel is used to transmit various commands to `init` (see `src/shared/inc/lxinitshared.h`), such as:

- `LxInitMessageInitialize`: Configure the distribution
- `LxInitMessageCreateSession`: Create a new session leader. See [session leader](session-leader.md)
- `LxInitMessageTerminateInstance`: Terminate the distribution