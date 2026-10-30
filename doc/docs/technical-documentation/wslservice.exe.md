# wslservice.exe

WslService is a session 0 service, running as SYSTEM. Its job is to manage WSL sessions, communicate with the WSL2 virtual machine and configure WSL distributions. 

## COM Interface

Clients can connect to WslService via its COM interface, ILxssUserSession. Its definition can be found in `src/windows/service/inc/wslservice.idl`.

When a COM client calls [CoCreateInstance()](https://learn.microsoft.com/windows/win32/api/combaseapi/nf-combaseapi-cocreateinstance) on this interface, the service receives the requests via `LxssUserSessionFactory` (see `src/windows/service/exe/LxssUserSessionFactory.cpp`) and returns an instance of `LxssUserSession` (see `src/windows/service/exe/LxssUserSession.cpp`) per Windows user (calling CoCreateInstance() multiple times from the same Windows user accounts returns the same instance).

The client can then use its `ILxssUserSession` instance to call methods into the service, such as:

- `CreateInstance()`: Launch a WSL distribution
- `CreateLxProcess()`: Launch a process inside a distribution
- `RegisterDistribution()`: Register a new WSL distribution
- `Shutdown()`: Terminate all WSL distributions

## WSL2 Virtual machine

WslService manages the WSL2 Virtual Machine. The virtual machine management logic can be found in `src/windows/service/exe/WslCoreVm.cpp`. 

Once booted, WslService maintains an [hvsocket](https://learn.microsoft.com/virtualization/hyper-v-on-windows/user-guide/make-integration-service) with the Virtual Machine which it uses to send various commands to Linux processes (see [mini_init](mini_init.md) for more details). 

### Large memory backing pages

To opt in to 2 MiB host backing pages for the WSL2 virtual machine, add the following to `%UserProfile%\.wslconfig`:

```ini
[experimental]
largeMemoryBackingPages=true
```

Run `wsl --shutdown` before restarting WSL to apply the change. The setting defaults to `false`, preserving the existing memory-density optimization on supported Windows builds.
Large backing pages may improve translation lookaside buffer (TLB) efficiency for memory-intensive workloads, but can increase host memory usage and allocation latency under memory pressure.
This controls host backing pages, not Linux huge-page allocation. When enabled, page reporting uses 2 MiB granularity and the small-page fault-cluster overrides are omitted.

WSLC C SDK clients can opt in independently by passing `WSLC_SESSION_FEATURE_FLAG_LARGE_MEMORY_BACKING_PAGES` to
`WslcSetSessionSettingsFeatureFlags` before creating a session. COM clients can set `WslcFeatureFlagsLargeMemoryBackingPages`
in `WSLCSessionSettings.FeatureFlags` or `WSLCCompatSessionSettings.FeatureFlags`.
The `.wslconfig` setting does not configure WSLC sessions.
The SDK flag requires WSL 3.1.0 or later; `WslcCreateSession` returns `WSLC_E_RUNTIME_UPDATE_NEEDED`
on older runtimes, with a localized installed/required-version message.

## WSL2 Distributions 

Once the virtual machine is running, WSL distributions can be started by calling `WslCoreVm::CreateInstance`. Each running distribution is represented by a `WslCoreInstance` (see `src/windows/service/exe/WslCoreInstance.cpp`).

Each `WslCoreInstance` maintains an hvsocket connection to [init](init.md) which allows WslService to perform various tasks such as:

- Launching processes inside the distribution
- Be notified when the distribution exits
- Mount drvfs shares (/mnt/*)
- Stop the distribution