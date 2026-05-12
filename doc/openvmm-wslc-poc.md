# WSLC with OpenVMM: Architecture & Proof of Concept

## 1. WSLC Architecture with HCS (Hyper-V Backed VM)

### Overview

WSLC (WSL Containers) runs Linux containers inside a lightweight Hyper-V virtual machine. The architecture has three main processes:

```
┌─────────────────┐     ┌──────────────────────┐     ┌──────────────────┐
│   wslc.exe      │     │  wslservice.exe       │     │ wslcsession.exe  │
│   (CLI client)  │────►│  (SYSTEM service)     │────►│ (per-user COM    │
│                 │ COM │                       │ COM │  server)         │
└─────────────────┘     │  Creates the VM via   │     │  Manages the VM  │
                        │  HcsVirtualMachine    │     │  lifecycle via   │
                        │                       │     │  WSLCVirtualMachine│
                        └──────────┬────────────┘     └────────┬─────────┘
                                   │                           │
                          HCS APIs │                  HvSocket │
                                   ▼                           ▼
                        ┌──────────────────────────────────────────────┐
                        │              Hyper-V VM                      │
                        │  ┌─────────┐  ┌───────────┐  ┌───────────┐  │
                        │  │mini_init│  │containerd  │  │  dockerd  │  │
                        │  │(vsock   │  │            │  │           │  │
                        │  │ port    │  │            │  │           │  │
                        │  │ 50000)  │  │            │  │           │  │
                        │  └─────────┘  └───────────┘  └───────────┘  │
                        └──────────────────────────────────────────────┘
```

### Key Components

**wslservice.exe (SYSTEM service)**
- Receives `CreateSession` requests from `wslc.exe` via COM (`IWSLCSessionManager`)
- Creates the VM by instantiating `HcsVirtualMachine`, which implements `IWSLCVirtualMachine`
- `HcsVirtualMachine` uses HCS (Host Compute Service) APIs to create, configure, and manage the Hyper-V VM
- Passes the `IWSLCVirtualMachine` interface to the per-user session process

**wslcsession.exe (per-user COM server)**
- Receives the `IWSLCVirtualMachine` interface from the service
- Wraps it in `WSLCVirtualMachine` (client-side class) which handles:
  - Connecting to mini_init via HvSocket port 50000
  - Forking sub-processes via the init channel protocol
  - Mounting VHDs (root filesystem, kernel modules, storage)
  - Configuring networking (launching GNS, DNS tunneling)
  - Starting containerd and dockerd
  - Docker API communication via `DockerHTTPClient`
- Manages container lifecycle (create, start, exec, stop, delete)

**HcsVirtualMachine (IWSLCVirtualMachine implementation)**
- `GetId()` — returns the VM GUID
- `AcceptConnection()` — listens on HvSocket port 50000 for mini_init
- `ConfigureNetworking()` — creates NatNetworking or VirtioNetworking engine
- `AttachDisk()` / `DetachDisk()` — hot-add/remove SCSI disks via `HcsModifyComputeSystem`
- `AddShare()` / `RemoveShare()` — hot-add/remove Plan9 or VirtioFS shares
- `GetTerminationEvent()` — event signaled when VM exits
- `ConnectToVsockPort()` — connects to a guest vsock port via HvSocket

### Communication Flow

1. **VM Creation**: `HcsCreateComputeSystem()` with JSON config (CPU, memory, kernel, initrd, SCSI disks, HvSocket security)
2. **VM Boot**: `HcsStartComputeSystem()` boots the Linux kernel
3. **Init Connection**: mini_init connects to HvSocket port 50000; session process accepts via `hvsocket::Listen` + `hvsocket::Accept`
4. **Sub-connections**: Fork returns a port number; session connects via `hvsocket::Connect(vmId, port)`
5. **Disk Hot-Add**: `HcsModifyComputeSystem()` with SCSI attachment request
6. **Share Hot-Add**: `HcsModifyComputeSystem()` with Plan9/VirtioFS share request, or via `GuestDeviceManager`
7. **Networking**: GNS daemon launched inside VM; socket handles passed to service-side networking engine

---

## 2. Adding OpenVMM Support: Process & Changes

### Approach

Introduced `OpenVmmVirtualMachine` — a new class implementing `IWSLCVirtualMachine` that spawns `openvmm.exe` as a child process instead of using HCS APIs. The session process (`wslcsession.exe`) is largely unchanged — it operates through the `IWSLCVirtualMachine` interface, which abstracts the VMM backend.

The backend is selected via user settings in `settings.yaml`:
- `session.openVmm: true` — enables OpenVMM in CLI mode (all config as command-line args)
- `session.openVmmTtrpc: true` — enables OpenVMM in ttrpc mode (VM configured via ttrpc RPCs, supports runtime disk hot-add/remove)

These settings map to `WslcFeatureFlagsOpenVmm` (32) and `WslcFeatureFlagsOpenVmmTtrpc` (64) feature flags in `wslc.idl`.

### Architecture with OpenVMM

```
┌─────────────────┐     ┌──────────────────────┐     ┌──────────────────┐
│   wslc.exe      │     │  wslservice.exe       │     │ wslcsession.exe  │
│   (CLI client)  │────►│  (SYSTEM service)     │────►│ (per-user COM    │
│                 │ COM │                       │ COM │  server)         │
└─────────────────┘     │  Creates VM via       │     │  Same as before  │
                        │  OpenVmmVirtualMachine│     │                  │
                        └──────────┬────────────┘     └────────┬─────────┘
                                   │                           │
                     Child process │               AF_UNIX     │
                     + ttrpc RPC   │              (via vsock    │
                                   │               bridge)     │
                                   ▼                           ▼
                        ┌──────────────────────────────────────────────┐
                        │            openvmm.exe (WHP VM)              │
                        │                                              │
                        │  ┌────────────────────────────────────────┐  │
                        │  │         hybrid_vsock bridge            │  │
                        │  │  (Unix domain socket at <vsock_path>)  │  │
                        │  └────────────────────────────────────────┘  │
                        │                                              │
                        │  ┌────────────────────────────────────────┐  │
                        │  │  ttrpc server (vmservice.VM)           │  │
                        │  │  (Unix domain socket at <ttrpc_path>)  │  │
                        │  └────────────────────────────────────────┘  │
                        │                                              │
                        │  ┌─────────┐  ┌───────────┐  ┌───────────┐  │
                        │  │mini_init│  │containerd  │  │  dockerd  │  │
                        │  └─────────┘  └───────────┘  └───────────┘  │
                        └──────────────────────────────────────────────┘
```

### Key Challenges & Solutions

#### Challenge 1: HvSocket Not Available with WHP
**Problem**: WHP VMs don't register with the Windows HvSocket driver, so all `hvsocket::Listen` and `hvsocket::Connect` calls fail with `0x80072741` ("address not valid").

**Solution**: OpenVMM provides a `hybrid_vsock` bridge via `--vmbus-vsock-path`. This creates a Unix domain socket on the host that relays connections between the host and guest vsock ports.

- **Guest-initiated connections** (e.g., mini_init connecting to port 50000): OpenVMM connects to `<vsock_path>_<GUID>` on the host. We pre-create a Unix socket listener at that path before launching OpenVMM.
- **Host-initiated connections** (e.g., Fork sub-connections): We connect to the main `<vsock_path>` Unix socket and send `CONNECT <port>\n`, receiving `OK <id>\n` in response.

#### Challenge 2: AF_UNIX Sockets Don't Support Overlapped I/O
**Problem**: `SocketChannel` (the WSL message protocol layer) uses `WSARecv` with overlapped I/O. Windows AF_UNIX sockets don't support this, causing `WSAEOPNOTSUPP` errors.

**Solution**: Introduced `SocketTransport` — an abstraction layer that handles overlapped vs. non-overlapped sockets transparently. `SocketChannel` now uses `SocketTransport` internally, and a factory function `CreateSocketTransport()` auto-detects the socket type:

- **`OverlappedSocketTransport`**: For AF_HYPERV, AF_INET sockets — delegates to `WSARecv`/`WSASend` with `OVERLAPPED` as before.
- **`SyncSocketTransport`**: For AF_UNIX sockets — uses `WSAEventSelect()` to put the socket in non-blocking mode, then loops with event waits to accumulate data (since `MSG_WAITALL` doesn't work with non-blocking sockets).

`ConnectToVsockPort` now returns the AF_UNIX socket directly (no TCP relay), and `SocketChannel` auto-detects the type via the transport-aware constructor.

#### Challenge 3: Kernel Format Incompatibility
**Problem**: The WSL kernel is a bzImage (MZ/PE header) that HCS handles natively. OpenVMM's Linux loader expects an uncompressed ELF vmlinux.

**Solution**: Extract the vmlinux from the bzImage. `OpenVmmVirtualMachine` looks for `vmlinux` in the tools directory, falling back to `kernel` if not found.

#### Challenge 4: Memory Allocation
**Problem**: OpenVMM on WHP allocates guest RAM upfront (unlike HCS which uses demand-paging). The default 32GB allocation fails with `ERROR_NO_SYSTEM_RESOURCES`.

**Solution**: Cap guest RAM at 4GB for the PoC. Future work: use `--shared-memory` mode or file-backed memory.

#### Challenge 5: Networking
**Problem**: HCS uses GNS (Guest Network Service) for networking. OpenVMM doesn't have GNS.

**Solution**: Use OpenVMM's built-in `consomme` NAT backend with `--virtio-net consomme`. A dedicated `ConsommeNetworking` class implements `INetworkingEngine` — its `Initialize()` is a no-op since consomme is self-contained (userspace TCP/IP stack inside the VMM with NAT, DHCP, and DNS). The guest receives its IP address via DHCP on `eth0`. `FillInitialConfiguration()` tells the guest init to enable DHCP client mode (`LxMiniInitNetworkingModeNat` + `EnableDhcpClient = true`).

#### Challenge 6: Disk Hot-Add
**Problem**: The storage VHDX needs to be attached at runtime.

**Solution**: Dual-mode approach:
- **CLI mode** (`session.openVmm: true`): Pre-create the VHDX in the `OpenVmmVirtualMachine` constructor and include it in the CLI arguments at boot time. `AttachDisk` returns the pre-allocated LUN for known paths. `ConfigureStorage` detects unformatted disks and formats them on first use. Swap VHD is also pre-created and pre-attached.
- **ttrpc mode** (`session.openVmmTtrpc: true`): A `TtrpcClient` connects to OpenVMM's vmservice Unix socket and calls `ModifyResource` RPCs for runtime `AttachDisk`/`DetachDisk`. This matches the HCS behavior more closely.

### Files Created/Modified

#### New Files (WSL)
| File | Description |
|---|---|
| `src/windows/service/exe/OpenVmmVirtualMachine.h` | Header for OpenVMM-backed `IWSLCVirtualMachine` implementation |
| `src/windows/service/exe/OpenVmmVirtualMachine.cpp` | Implementation: process management, CLI building, vsock bridge, disk pre-attach, ttrpc integration |
| `src/windows/service/exe/TtrpcClient.h` | Header for ttrpc (transport RPC) client for OpenVMM's vmservice |
| `src/windows/service/exe/TtrpcClient.cpp` | ttrpc wire protocol + manual protobuf encoding for `CreateVM`, `ResumeVM`, `WaitVM`, `TeardownVM`, `ModifyResource` RPCs |
| `src/windows/common/SocketTransport.h` | `SocketTransport` abstraction for overlapped vs. non-overlapped socket I/O |
| `src/windows/common/SocketTransport.cpp` | `OverlappedSocketTransport` (AF_HYPERV/AF_INET) and `SyncSocketTransport` (AF_UNIX) implementations |
| `src/windows/common/ConsommeNetworking.h` | Header for consomme NAT networking engine |
| `src/windows/common/ConsommeNetworking.cpp` | `INetworkingEngine` implementation for OpenVMM's built-in consomme NAT/DHCP backend |

#### Modified Files (WSL)
| File | Change |
|---|---|
| `src/windows/service/inc/wslc.idl` | Added `ConnectToVsockPort` and `AcceptCrashDumpConnection` methods to `IWSLCVirtualMachine`; added `WslcFeatureFlagsOpenVmm` (32) and `WslcFeatureFlagsOpenVmmTtrpc` (64) feature flags; added `WSLCNetworkingModeConsomme` |
| `src/windows/service/exe/HcsVirtualMachine.h/.cpp` | Added `ConnectToVsockPort` (delegates to `hvsocket::Connect`) and `AcceptCrashDumpConnection` (HvSocket listener on port 50005) implementations |
| `src/windows/service/exe/WSLCSessionManager.cpp` | Backend selection via `WslcFeatureFlagsOpenVmm` feature flag; disables GPU/VirtioFS/DNS tunneling; forces `WSLCNetworkingModeConsomme` |
| `src/windows/service/exe/CMakeLists.txt` | Added new source/header files |
| `src/windows/common/WSLCUserSettings.h/.cpp` | Added `SessionOpenVmm` (`session.openVmm`) and `SessionOpenVmmTtrpc` (`session.openVmmTtrpc`) settings |
| `src/windows/common/CMakeLists.txt` | Added `SocketTransport.cpp`, `ConsommeNetworking.cpp` and headers |
| `src/windows/wslcsession/WSLCVirtualMachine.h/.cpp` | Added `Vm()` accessor; crash dump collection via `AcceptCrashDumpConnection`; port forwarding TODO for OpenVMM; mount failure recovery for pre-attached disks |
| `src/windows/wslcsession/WSLCSession.cpp` | Mount failure + format retry for pre-attached unformatted disks |
| `src/windows/wslcsession/DockerHTTPClient.h/.cpp` | Replaced `GUID VmId` + `hvsocket::Connect` with `IWSLCVirtualMachine*` + `ConnectToVsockPort` |
| `src/shared/inc/SocketChannel.h` | Added transport-aware constructor that auto-detects AF_UNIX sockets and uses `SyncSocketTransport`; integrated `SocketTransport` into send/receive paths |
| `src/shared/inc/socketshared.h` | Extended socket helpers to support `SocketTransport`-based I/O |
| `src/shared/inc/lxinitshared.h` | Added networking configuration definitions for consomme mode (`LxMiniInitNetworkingModeNat`, DHCP enable flag) |
| `src/linux/init/WSLCInit.cpp` | Guest-side changes for consomme networking (DHCP client configuration on boot) |

---

## 3. Current Gaps: OpenVMM vs HCS

### Functional Gaps

| Feature | HCS | OpenVMM PoC | Notes |
|---|---|---|---|
| VM creation | HCS APIs (JSON config) | CLI args or ttrpc `CreateVM` RPC | ttrpc mode uses `TtrpcClient` with manual protobuf encoding |
| Kernel format | bzImage (native) | vmlinux (ELF only) | Need bzImage support in OpenVMM's loader |
| Guest RAM | Demand-paged, up to host RAM | Upfront allocation, capped at 4GB | Use `--shared-memory` or file-backed memory |
| HvSocket | Native via hvsocket driver | Unix socket bridge + `SocketTransport` | AF_UNIX sockets handled via `SyncSocketTransport` (no TCP relay) |
| SCSI disk hot-add | `HcsModifyComputeSystem` | ttrpc: runtime via `ModifyResource` RPC; CLI: pre-attach at boot | ttrpc mode matches HCS behavior |
| SCSI disk hot-remove | `HcsModifyComputeSystem` | ttrpc: runtime via `ModifyResource` RPC; CLI: not implemented | ttrpc mode matches HCS behavior |
| Plan9 share hot-add | `HcsModifyComputeSystem` | Not implemented | Needs OpenVMM proto extension |
| VirtioFS share hot-add | `GuestDeviceManager` | Not implemented | Needs OpenVMM VirtioFS hot-add support |
| Share hot-remove | HCS/GuestDeviceManager | Not implemented | Needs OpenVMM proto extension |
| GPU passthrough | HCS GPU modify | Not supported | OpenVMM lacks Windows GPU assignment |
| Networking | GNS + NAT/VirtioProxy | consomme (DHCP/NAT inside VMM) | `ConsommeNetworking` engine; guest uses DHCP on eth0 |
| DNS tunneling | GNS socket passthrough | Not supported | consomme has built-in DNS but no host DNS integration |
| Crash dump collection | HvSocket port 50005 | Unix socket via hybrid_vsock bridge | `AcceptCrashDumpConnection` implemented for both backends |
| Serial/dmesg console | COM0 + VirtioSerial hvc0 | `DmesgCollector` via named pipes (earlycon on COM1 + hvc0) | Captures kernel output from boot via earlycon |
| VM saved state (.vmrs) | HCS bugcheck dump | Not supported | Could use OpenVMM save/restore |
| Container stdout relay | Works | Race condition for fast-exiting containers | Timing issue in pipe relay |
| Swap VHD | Hot-attached + mkswap | CLI: pre-attached (ephemeral); ttrpc: hot-attached | CLI mode pre-creates and deletes each session |
| Port forwarding | `PortRelayHandle.cpp` via hvsocket | Not supported | Consomme backend needs port forwarding design |

### Architecture Gaps

| Area | Gap | Remediation |
|---|---|---|
| **ttrpc client** | Manual protobuf encoding; no external proto dependency | Consider code-generating from `.proto` files if scope grows |
| **Process model** | openvmm runs as child of wslservice (SYSTEM) | Consider security implications; may need separate process identity |
| **Socket path length** | Unix domain socket paths limited to 108 bytes | Using short paths under `C:\ProgramData\wslc\` (already mitigated) |
| **Port forwarding** | `PortRelayHandle.cpp` still uses `hvsocket::Connect` | Convert to `ConnectToVsockPort` or design consomme port forwarding |
| **Kernel modules** | Loaded from modules.vhd (works) | Same as HCS |
| **ARM64** | Not tested | OpenVMM supports ARM64 UEFI boot |

### Quality Gaps

| Area | Status |
|---|---|
| Error handling | Basic; some paths return E_NOTIMPL (shares) |
| Logging | `DmesgCollector` via named pipes + ETW traces |
| Testing | Manual only; no automated tests |
| Cleanup | vsock/ttrpc socket files cleaned on shutdown; may leak on crash |
| Security | AF_UNIX sockets used directly (no TCP relay); openvmm in job object with `KILL_ON_JOB_CLOSE` |
| Performance | `SyncSocketTransport` adds event-wait overhead vs overlapped I/O; upfront RAM allocation limits density |

---

## 4. Recommended Next Steps

1. **Add bzImage support** to OpenVMM's Linux loader to avoid the vmlinux extraction step
2. **Use shared memory mode** (`--shared-memory`) to support larger guest RAM without upfront allocation
3. **Extend OpenVMM proto** with Plan9/VirtioFS share hot-add/remove for `AddShare`/`RemoveShare`
4. **Implement port forwarding** for the consomme networking backend
5. **Investigate container stdout** relay timing issue for fast-exiting containers
6. **Add automated tests** using the existing WSLC TAEF test framework
7. **Security review** of the openvmm process model and AF_UNIX socket permissions
8. **DNS tunneling** integration with host DNS resolution via consomme
