---
description: 'WSL architecture overview — component map, boot sequence, communication patterns, and key source files'
applyTo: 'src/**'
---

# WSL Architecture

Reference: https://wsl.dev/technical-documentation/

WSL is composed of Windows executables, Linux binaries, COM interfaces, and hvsocket communication channels.

## Component Diagram

Source: https://wsl.dev/technical-documentation/ (Mermaid flowchart)

```
┌─────────────────────────── Windows ───────────────────────────────┐
│                                                                    │
│  C:\Windows\System32\wsl.exe ──CreateProcess()──► wsl.exe         │
│                                                     │ COM         │
│  wslg.exe ──────────────── COM ──────────────►  wslservice.exe    │
│  wslapi.dll ──────────────── COM ──────────────►    │             │
│    ▲ LoadLibrary()                                  │             │
│  debian.exe, ubuntu.exe, ...            CreateProcessAsUser()     │
│                                          ┌──────────┴──────────┐  │
│  Windows filesystem                 wslrelay.exe    wslhost.exe   │
│  (//wsl.localhost)                                                │
│        │                                                          │
└────────┼──────────────────────────────────────────────────────────┘
         │ hvsocket              │ hvsocket          │ hvsocket
         │                       │                   │
┌────────┼───────────────────────┼───────────────────┼──── Linux ───┐
│        │                       │                   │              │
│        │              mini_init ◄──────────────────┘              │
│        │                │  exec()    │ exec()    │ exec()         │
│        │               gns          init       localhost          │
│        │                             │                            │
│        │          ┌── Linux Distribution ──────────────────┐      │
│        │          │  init ──exec()──► plan9                │      │
│        ▼          │    │──exec()──► session leader          │      │
│      plan9 ◄──────│    │             │──exec()──► relay     │      │
│                   │    │                          │         │      │
│                   │    │               fork(),exec()        │      │
│                   │    │                   ▼                │      │
│                   │    │           User command (bash)      │      │
│                   └────────────────────────────────────────┘      │
└───────────────────────────────────────────────────────────────────┘

Communication: wsl.exe ◄──hvsocket──► relay (stdin/stdout/stderr)
               wslservice.exe ◄──hvsocket──► mini_init, gns, init
               Windows filesystem ◄──hvsocket──► plan9
```

## Windows Components

| Component | Source | Purpose | Docs |
|-----------|--------|---------|------|
| **wsl.exe** | `src/windows/wsl/` | Main CLI entrypoint. Parses args (`src/windows/common/wslclient.cpp`), calls wslservice via COM (`src/windows/common/svccomm.cpp`), relays stdin/stdout/stderr (`src/windows/common/relay.cpp`). | [wsl.exe](https://wsl.dev/technical-documentation/wsl.exe/) |
| **wslservice.exe** | `src/windows/service/` | Session 0 service (SYSTEM). Manages WSL sessions, VM lifecycle, distribution registration. COM interface: `ILxssUserSession` (defined in `src/windows/service/inc/wslservice.idl`). | [wslservice.exe](https://wsl.dev/technical-documentation/wslservice.exe/) |
| **wslhost.exe** | `src/windows/wslhost/` | Desktop notifications (`src/windows/common/notifications.cpp`) and background Linux process lifetime management. | [wslhost.exe](https://wsl.dev/technical-documentation/wslhost.exe/) |
| **wslrelay.exe** | `src/windows/wslrelay/` | Relays localhost network traffic (NAT mode) and debug console output from Linux to Windows. | [wslrelay.exe](https://wsl.dev/technical-documentation/wslrelay.exe/) |
| **wslg.exe** | `src/windows/wslg/` | Like wsl.exe but Win32 (not console) — for graphical Linux apps without a console window. | [wslg.exe](https://wsl.dev/technical-documentation/wslg.exe/) |
| **wslconfig.exe** | (no standalone source directory) | Configure WSL distributions. Functionality integrated with other WSL executables. | [wslconfig.exe](https://wsl.dev/technical-documentation/wslconfig.exe/) |
| **wslapi.dll / libwsl** | `src/windows/libwsl/` | Public WSL API DLL (used by distribution launchers like ubuntu.exe). | [wslapi.dll](https://learn.microsoft.com/windows/win32/api/wslapi/) |
| **wslsettings** | `src/windows/wslsettings/` | WinUI 3 / C# settings app (MVVM, .NET 8.0, WindowsAppSDK). | — |
| **wslinstall** | `src/windows/wslinstall/` | Installation executables. | — |

## Linux Components

| Component | Source | Purpose | Docs |
|-----------|--------|---------|------|
| **mini_init** | `src/linux/init/` (entrypoint) | First process in WSL2 VM. Mounts filesystems, configures logging, connects hvsockets to wslservice, launches gns and distributions. | [mini_init](https://wsl.dev/technical-documentation/mini_init/) |
| **gns** | `src/linux/init/GnsEngine.cpp` | Networking configuration (IP, routing, DNS, MTU). Maintains hvsocket to wslservice. Handles DNS tunneling. | [gns](https://wsl.dev/technical-documentation/gns/) |
| **init** | `src/linux/init/` | Top-level process per distribution. Mounts /proc, /sys, /dev; configures cgroups; registers binfmt; starts systemd; mounts drvfs; creates session leaders. | [init](https://wsl.dev/technical-documentation/init/) |
| **session leader** | `src/linux/init/` | Forked from init. Creates Linux processes on behalf of users. Each is associated with a Windows console. | [session leader](https://wsl.dev/technical-documentation/session-leader/) |
| **relay** | `src/linux/init/` | Created by session leader (WSL2). Creates hvsocket channels for stdin/stdout/stderr relay to wsl.exe. Forks into user process. | [relay](https://wsl.dev/technical-documentation/relay/) |
| **plan9** | `src/linux/init/plan9.cpp` (server entrypoint) + `src/linux/plan9/` (protocol implementation) | Plan9 filesystem server for accessing Linux files from Windows (`\\wsl.localhost\<distro>`). Windows-side uses p9rdr.sys redirector driver. | [plan9](https://wsl.dev/technical-documentation/plan9/) |
| **localhost** | `src/linux/init/localhost.cpp` | Forwards network traffic between WSL2 VM and Windows. NAT: watches TCP ports. Mirrored: BPF intercepts bind(). | [localhost](https://wsl.dev/technical-documentation/localhost/) |

## Shared Code

- `src/shared/` — Cross-platform code (config file parsing, message definitions)
- `src/shared/inc/lxinitshared.h` — Message definitions for mini_init, init, gns channels

## Communication Patterns

- **Windows ↔ Linux**: hvsocket channels (Hyper-V sockets)
- **Windows clients ↔ wslservice**: COM (`ILxssUserSession`)
- **Linux files → Windows**: Plan9 protocol via p9rdr.sys (`\\wsl$`, `\\wsl.localhost`)
- **Windows drives → Linux**: drvfs mounts under `/mnt/` (plan9/virtio-plan9/virtiofs per config)

## WSL2 Boot Sequence

1. `wsl.exe` → calls `CreateInstance()` on `wslservice.exe` via COM
2. `wslservice.exe` → creates VM via Host Compute System (HCS) (`WslCoreVm.cpp`, `HcsCreateComputeSystem()`)
3. VM kernel boots → executes `mini_init` from initramfs
4. `mini_init` → mounts filesystems, receives `LxMiniInitMessageEarlyConfig` via hvsocket, launches `gns`
5. `wslservice.exe` sends `LxMiniInitMessageLaunchInit` → `mini_init` mounts distro VHD, launches `init`
6. `init` → configures distribution (cgroups, binfmt, systemd, drvfs), connects hvsocket to wslservice
7. `wslservice.exe` sends `LxInitMessageCreateSession` → `init` forks session leader
8. Session leader forks `relay` → relay creates hvsocket channels for stdio, forks user process (bash)
9. `wsl.exe` receives stdio hvsocket handles, relays terminal I/O

## Linux Entrypoint Dispatcher

The `/init` binary serves multiple roles. `main()` in `src/linux/init/main.cpp` calls `WslEntryPoint()` in `src/linux/init/init.cpp`, which checks `argv[0]` (as `Argv[]`) to determine which logic to run:
- `init` → distribution init
- `plan9` → Plan9 filesystem server
- `localhost` → network forwarding
- `mount.drvfs` → drive mounting (`MountDrvfsEntry()` in `src/linux/init/drvfs.cpp`)
- Anything else → Windows interop (binfmt, `src/linux/init/binfmt.cpp`)

See `WslEntryPoint()` in `src/linux/init/init.cpp`.

## Windows Interop (Running Windows Executables from Linux)

- Binfmt interpreter registered at `/proc/sys/fs/binfmt_misc/WSLInterop`, points to `/init`
- Interop servers listen at unix sockets under `/run/WSL/`, discovered via `$WSL_INTEROP` env var
- `/init` sends `LxInitMessageCreateProcess` (WSL1) or `LxInitMessageCreateProcessUtilityVm` (WSL2) to Windows

## Systemd Integration

- Enabled via `/etc/wsl.conf` → `[boot] systemd=true`
- When enabled, `init` forks: parent runs `/sbin/init` (systemd) as PID 1, child continues WSL config
- WSL creates systemd config files under `/run` to protect binfmt interpreter and X11 socket

## Drvfs (Accessing Windows Drives from Linux)

- WSL maintains two mount namespaces per distribution: elevated and non-elevated
- Plan9 file server started per session leader, mounted by `init`
- Manual mounting: `mount -t drvfs C: /tmp/my-mount-point`

## Key Source Files

| File | Purpose |
|------|---------|
| `src/windows/service/exe/WslCoreVm.cpp` | VM management |
| `src/windows/service/exe/LxssUserSession.cpp` | COM session (per Windows user) |
| `src/windows/service/exe/LxssUserSessionFactory.cpp` | COM factory |
| `src/windows/service/exe/WslCoreInstance.cpp` | Per-distribution instance |
| `src/windows/service/exe/DistributionRegistration.cpp` | Distribution registry lookup |
| `src/windows/service/exe/Lifetime.cpp` | Process lifetime association |
| `src/windows/common/GnsChannel.cpp` | Windows-side networking channel |
| `src/windows/common/wslclient.cpp` | CLI argument parsing |
| `src/windows/common/svccomm.cpp` | COM communication helpers |
| `src/windows/common/relay.cpp` | Stdio relay logic |
| `src/windows/common/hcs_schema.h` | HCS VM JSON schema |
| `src/shared/inc/lxinitshared.h` | Message definitions |
| `src/linux/init/main.cpp` | Linux main() entrypoint |
| `src/linux/init/init.cpp` | WslEntryPoint() — argv[0] dispatcher |
| `src/linux/init/GnsEngine.cpp` | Linux-side networking |
| `src/linux/init/plan9.cpp` | Plan9 filesystem server |
| `src/linux/init/binfmt.cpp` | Windows interop |
| `src/linux/init/drvfs.cpp` | Drive mounting |
| `src/linux/init/localhost.cpp` | Localhost network forwarding |
