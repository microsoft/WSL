# wslservice.exe

WslService is a session 0 service, running as SYSTEM. Its job is to manage WSL sessions, communicate with the WSL2 virtual machine and configure WSL distributions. 

## COM Interface

Clients can connect to WslService via its COM interface, ILxssUserSession. Its definition can be found in `src/windows/service/inc/wslservice.idl`.

When a COM client calls [CoCreateInstance()](https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-cocreateinstance) on this interface, the service receives the requests via `LxssUserSessionFactory` (see `src/windows/service/LxssUserSessionFactory.cpp`) and returns an instance of `LxssUserSession` (see `src/windows/service/LxssUserSession.cpp`) per Windows user (calling CoCreateInstance() multiple times from the same Windows user accounts returns the same instance).

The client can then use its `ILxssUserSession` instance to call methods into the service, such as:

- `CreateInstance()`: Launch a WSL distribution
- `CreateLxProcess()`: Launch a process inside a distribution
- `RegisterDistribution()`: Register a new WSL distribution
- `Shutdown()`: Terminate all WSL distributions

## WSL2 Virtual machine

WslService manages the WSL2 Virtual Machine. The virtual machine management logic can be found in `src/windows/service/WslCoreVm.cpp`. 

Once booted, WslService maintains an [hvsocket](https://learn.microsoft.com/virtualization/hyper-v-on-windows/user-guide/make-integration-service) with the Virtual Machine which it uses to send various commands to Linux processes (see [mini_init](mini_init.md) for more details). 

### Experimental VM backend selection

`LxssUserSessionImpl::_CreateVm()` is the centralized creation and selection point for
WSL2. The session owns an `IWslCoreVm`; `WslCoreVm` implements HCS and
`OpenVmmWslCoreVm` implements OpenVMM. There is no separate factory class or shared
`WslCoreVm` facade. Each implementation owns its guest channels, devices, and
cleanup. This selection does not apply to WSLC sessions or WSL1 distributions.

HCS remains the default. OpenVMM requires a build configured with
`-DINCLUDE_OPENVMM=ON` and explicit opt-in in the user's `.wslconfig`:

```ini
[experimental]
openVmm=true
```

| Configuration or event | Behavior |
| --- | --- |
| `openVmm` absent or `false` | Create an HCS VM. |
| `openVmm=true`, OpenVMM included in the build | Create an OpenVMM VM. Missing/broken runtime artifacts are startup failures, not a reason to select HCS. |
| `openVmm=true`, OpenVMM excluded from the build | Return the localized unavailable-backend error (`E_NOTIMPL`). Do not fall back. |
| Invalid boolean value | Emit a configuration warning and retain the current parsed value, initially `false`. |
| OpenVMM initialization fails | Clean up the failed creation, clear the recorded VM identity/backend, and propagate the error. Do not retry with HCS. |
| Setting changes while a VM is running | Keep using that VM. Normal and forced shutdown target the backend recorded at creation, not the newly edited setting. |
| User disables OpenVMM and shuts down WSL | The next VM creation reads the updated configuration and uses HCS. |

Rollback is explicit: set `openVmm=false` (or remove the setting), then run
`wsl --shutdown` before launching again. Shutdown terminates all running WSL
distributions for the user. Automatic fallback is intentionally not provided:
silently changing backends can change filesystem/networking semantics, and
retrying after partial initialization risks overlapping resources.

### OpenVMM RPC failure and process lifetime

The private RPC client uses bounded gRPC over AF_UNIX. It never replays an
uncertain mutation. The backend routes resume, disk, share, and localhost-port
operations through one service-side failure policy:

| Failure category | HRESULT examples | Backend action |
| --- | --- | --- |
| Validation | `E_INVALIDARG`, `ERROR_NOT_FOUND`, `ERROR_ALREADY_EXISTS`, `E_NOTIMPL` | Return the rejection; permit a corrected explicit request. |
| Authorization | `E_ACCESSDENIED`, `ERROR_LOGON_FAILURE` | Return the rejection; permit a corrected explicit request. |
| Timeout | `WAIT_TIMEOUT` | Cancel requests and retire the process. |
| Cancellation | `E_ABORT` | Cancel requests and retire the process. |
| Transport / protocol | `ERROR_CONNECTION_ABORTED`, `ERROR_INVALID_DATA` | Cancel requests and retire the process. |
| Invalid state | `ERROR_INVALID_STATE` | Cancel requests and retire the process. |
| Other server or resource failure | `E_FAIL`, `ERROR_RETRY`, `ERROR_NOT_ENOUGH_MEMORY`, other failing HRESULTs | Cancel requests and retire the process. |

Win32 names in the table denote HRESULTs constructed with `HRESULT_FROM_WIN32`.
The service intentionally makes a more conservative decision than the RPC
client: the private ABI does not reveal whether a timeout occurred before
dispatch, or whether `ERROR_INVALID_STATE` denotes a definite server rejection
or an already-invalidated client. Both require process recreation at this layer.
The original operation's HRESULT is returned; no retry or HCS fallback occurs.
Process exit retires the owning session VM. A subsequent launch constructs a new
configuration, process, VM identity, RPC handle, and resource bookkeeping.

Normal shutdown cancels mutations before joining the port-tracker and VirtioFS
workers. Forced shutdown cancels through the VM-ID process registration before
terminating that exact process. Unexpected process exit also cancels requests.
Cancellation can run concurrently with an RPC: both hold a shared handle-lifetime
lock, while publication and destruction hold it exclusively. The RPC client
continues to own serialization and deadlines. If exit races `CreateVm`, the newly
published handle is cancelled and initialization fails; a creation already in
flight without a handle remains bounded by its creation deadline.

Shutdown still permits bounded teardown/quit against a live process, followed by
timed process termination. Failed initialization uses the same destructor path,
rather than freeing an RPC handle ahead of its workers. Process-wait callbacks
finish accessing the backend before notifying the session, so draining them
cannot deadlock with destruction. Session notifications use weak ownership and
check the VM identity under the session lock: a delayed notification cannot stop
a replacement VM.

The existing WSL trace provider records process start/exit, initialization
failure, forced shutdown, RPC operation/result/category/elapsed time, and
recovery-required decisions, using the VM ID and process ID where applicable.
RPC payloads and server error text are not added to these events. Low-level Rust
tracing remains debugger output, not a new ETW provider or a new WPR profile.
Endpoint cleanup errors and process-termination timeouts are logged explicitly.
This is process/RPC diagnostics, not crash-artifact or kernel-panic collection.

## WSL2 Distributions 

Once the virtual machine is running, WSL distributions can be started by calling `WslCoreVm::CreateInstance`. Each running distribution is represented by a `WslCoreInstance` (see `src/windows/service/WslCoreInstance.cpp`).

Each `WslCoreInstance` maintains an hvsocket connection to [init](init.md) which allows WslService to perform various tasks such as:

- Launching processes inside the distribution
- Be notified when the distribution exits
- Mount drvfs shares (/mnt/*)
- Stop the distribution