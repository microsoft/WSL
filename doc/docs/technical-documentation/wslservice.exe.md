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

## WSL2 Distributions 

Once the virtual machine is running, WSL distributions can be started by calling `WslCoreVm::CreateInstance`. Each running distribution is represented by a `WslCoreInstance` (see `src/windows/service/WslCoreInstance.cpp`).

Each `WslCoreInstance` maintains an hvsocket connection to [init](init.md) which allows WslService to perform various tasks such as:

- Launching processes inside the distribution
- Be notified when the distribution exits
- Mount drvfs shares (/mnt/*)
- Stop the distribution