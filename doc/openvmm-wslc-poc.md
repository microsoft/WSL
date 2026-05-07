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
                     Child process │              Unix socket  │
                     + stdin pipe  │              + TCP relay   │
                                   ▼                           ▼
                        ┌──────────────────────────────────────────────┐
                        │            openvmm.exe (WHP VM)              │
                        │                                              │
                        │  ┌────────────────────────────────────────┐  │
                        │  │         hybrid_vsock bridge            │  │
                        │  │  (Unix domain socket at <vsock_path>)  │  │
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

**Solution**: TCP loopback relay — create a `localhost` TCP socket pair and relay data between the Unix socket and TCP socket in a background thread. The TCP socket supports overlapped I/O.

```
Unix socket ◄──► [Relay Thread] ◄──► TCP socket (loopback)
(from OpenVMM)    copies data         (returned to caller)
                  both directions     supports overlapped I/O ✓
```

#### Challenge 3: Kernel Format Incompatibility
**Problem**: The WSL kernel is a bzImage (MZ/PE header) that HCS handles natively. OpenVMM's Linux loader expects an uncompressed ELF vmlinux.

**Solution**: Extract the vmlinux from the bzImage. `OpenVmmVirtualMachine` looks for `vmlinux` in the tools directory, falling back to `kernel` if not found.

#### Challenge 4: Memory Allocation
**Problem**: OpenVMM on WHP allocates guest RAM upfront (unlike HCS which uses demand-paging). The default 32GB allocation fails with `ERROR_NO_SYSTEM_RESOURCES`.

**Solution**: Cap guest RAM at 4GB for the PoC. Future work: use `--shared-memory` mode or file-backed memory.

#### Challenge 5: Networking
**Problem**: HCS uses GNS (Guest Network Service) for networking. OpenVMM doesn't have GNS.

**Solution**: Use OpenVMM's built-in `consomme` NAT backend with `--virtio-net consomme`. Set `NetworkingMode = None` to skip GNS, and statically configure the guest's network interface and DNS via a shell command after boot.

#### Challenge 6: Disk Hot-Add
**Problem**: The storage VHDX needs to be attached at runtime, but the REPL-based hot-add approach doesn't work (rustyline doesn't read from piped stdin).

**Solution**: Pre-create the VHDX in the `OpenVmmVirtualMachine` constructor and include it in the CLI arguments at boot time. `AttachDisk` returns the pre-allocated LUN for known paths. `ConfigureStorage` detects unformatted disks and formats them on first use.

### Files Created/Modified

#### New Files (WSL)
| File | Description |
|---|---|
| `src/windows/service/exe/OpenVmmVirtualMachine.h` | Header for OpenVMM-backed `IWSLCVirtualMachine` implementation |
| `src/windows/service/exe/OpenVmmVirtualMachine.cpp` | Implementation: process management, CLI building, vsock bridge, disk pre-attach |

#### Modified Files (WSL)
| File | Change |
|---|---|
| `src/windows/service/inc/wslc.idl` | Added `ConnectToVsockPort` method to `IWSLCVirtualMachine` |
| `src/windows/service/exe/HcsVirtualMachine.h/.cpp` | Added `ConnectToVsockPort` implementation (delegates to `hvsocket::Connect`) |
| `src/windows/service/exe/WSLCSessionManager.cpp` | Backend selection via `WSLC_USE_OPENVMM=1` env var; feature flag clearing |
| `src/windows/service/exe/CMakeLists.txt` | Added new source/header files |
| `src/windows/wslcsession/WSLCVirtualMachine.h/.cpp` | Added `Vm()` accessor; crash dump graceful skip; DNS configuration; mount failure recovery; replaced `hvsocket::Connect` with `ConnectToVsockPort` in Fork/ConnectSocket |
| `src/windows/wslcsession/WSLCSession.cpp` | Mount failure + format retry for pre-attached disks |
| `src/windows/wslcsession/DockerHTTPClient.h/.cpp` | Replaced `GUID VmId` + `hvsocket::Connect` with `IWSLCVirtualMachine*` + `ConnectToVsockPort` |

---

## 3. Current Gaps: OpenVMM vs HCS

### Functional Gaps

| Feature | HCS | OpenVMM PoC | Notes |
|---|---|---|---|
| VM creation | HCS APIs (JSON config) | CLI args to openvmm.exe | Could use gRPC `CreateVM` for more control |
| Kernel format | bzImage (native) | vmlinux (ELF only) | Need bzImage support in OpenVMM's loader |
| Guest RAM | Demand-paged, up to host RAM | Upfront allocation, capped at 4GB | Use `--shared-memory` or file-backed memory |
| HvSocket | Native via hvsocket driver | Unix socket bridge + TCP relay | Works but adds latency from relay thread |
| SCSI disk hot-add | `HcsModifyComputeSystem` | Pre-attach at boot only | Implement gRPC `ModifyResource` client for runtime |
| SCSI disk hot-remove | `HcsModifyComputeSystem` | Not implemented | Implement gRPC `ModifyResource` client |
| Plan9 share hot-add | `HcsModifyComputeSystem` | Not implemented | Needs OpenVMM proto extension |
| VirtioFS share hot-add | `GuestDeviceManager` | Not implemented | Needs OpenVMM VirtioFS hot-add support |
| Share hot-remove | HCS/GuestDeviceManager | Not implemented | Needs OpenVMM proto extension |
| GPU passthrough | HCS GPU modify | Not supported | OpenVMM lacks Windows GPU assignment |
| Networking | GNS + NAT/VirtioProxy | consomme (static IP) | No port forwarding, no DNS tunneling |
| DNS tunneling | GNS socket passthrough | Static resolv.conf | consomme DNS works but no integration with host DNS |
| Crash dump collection | HvSocket port 50005 | Disabled (graceful skip) | Could use vsock bridge |
| Serial/dmesg console | COM0 + VirtioSerial hvc0 | Log file redirect | hvc1 logging not available |
| VM saved state (.vmrs) | HCS bugcheck dump | Not supported | Could use OpenVMM save/restore |
| Container stdout relay | Works | Race condition for fast-exiting containers | Timing issue in pipe relay |
| Swap VHD | Hot-attached + mkswap | Not supported (AttachDisk E_NOTIMPL for new disks) | Pre-attach or gRPC |

### Architecture Gaps

| Area | Gap | Remediation |
|---|---|---|
| **gRPC client** | No runtime VM management API | Implement C++ gRPC client using `vmservice.proto` |
| **Process model** | openvmm runs as child of wslservice (SYSTEM) | Consider security implications; may need separate process identity |
| **Socket path length** | Unix domain socket paths limited to 108 bytes | Use short paths under `C:\ProgramData\wslc\` |
| **Relay thread overhead** | Each vsock connection creates a TCP relay thread | Could use IO completion ports or async relay |
| **Port forwarding** | `PortRelayHandle.cpp` still uses `hvsocket::Connect` | Convert to `ConnectToVsockPort` |
| **Kernel modules** | Loaded from modules.vhd (works) | Same as HCS |
| **ARM64** | Not tested | OpenVMM supports ARM64 UEFI boot |

### Quality Gaps

| Area | Status |
|---|---|
| Error handling | Basic; some paths return E_NOTIMPL |
| Logging | Diagnostic log file + ETW traces |
| Testing | Manual only; no automated tests |
| Cleanup | vsock socket files may leak on crash |
| Security | TCP loopback relay binds to localhost (acceptable for PoC) |
| Performance | Relay threads add latency; upfront RAM allocation limits density |

---

## 4. Recommended Next Steps

1. **Implement gRPC client** for `AttachDisk`/`DetachDisk` to enable runtime disk management
2. **Add bzImage support** to OpenVMM's Linux loader to avoid the vmlinux extraction step
3. **Use shared memory mode** (`--shared-memory`) to support larger guest RAM without upfront allocation
4. **Extend OpenVMM proto** with Plan9/VirtioFS share hot-add/remove for `AddShare`/`RemoveShare`
5. **Convert remaining `hvsocket::Connect`** calls (PortRelayHandle) to `ConnectToVsockPort`
6. **Investigate container stdout** relay timing issue for fast-exiting containers
7. **Add automated tests** using the existing WSLC TAEF test framework
8. **Security review** of the TCP loopback relay and openvmm process model
