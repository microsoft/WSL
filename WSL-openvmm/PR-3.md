# PR 3 - HCS backend isolation

[Back to the implementation tracker](../WSL-openvmm.md)

**Status:** B1, B2, and B6 implemented under the accepted revised scope.
**Dependency:** PR 1.
**Branch:** `user/damanmmulye/wsl-openvmm-refactor`.
**Follow-up commit:** `b956db18`.
**Source evidence:** S1 and S11 in the main tracker.

## Accepted ownership boundary

`IWslCoreVm` is the service-facing backend contract. `WslCoreVm` remains the HCS
implementation; OpenVMM is a sibling implementation, not a backend underneath a
shared `WslCoreVm` facade. This explicitly replaces the original lower-level
extraction required by B1/B2.

`LxssUserSession` owns session policy, implementation selection, and the
`std::unique_ptr<IWslCoreVm>`. Each implementation owns VM initialization, guest
devices/channels, callbacks, and shutdown. HCS JSON generation, networking,
VirtioFS orchestration, and HCS resource cleanup stay together in `WslCoreVm`;
destruction through the virtual interface destructor preserves its existing
cleanup.

The refactor moves forced termination behind `WslCoreVm::ForceTerminate` and
routes import/export tar/error sockets through `ConnectToGuest`, whose HCS
implementation still calls the same HvSocket helper.

Keep OpenVMM implementation out of this PR. Its review question is: does the
abstraction preserve HCS behavior?

## Regression coverage

B6 is marked implemented because the HCS implementation and telemetry call sites
are retained, lifecycle coverage is added, and existing tests cover additional
paths. This does not claim passing CI results or exhaustive regression detection.
Telemetry preservation is supported by source review, not runtime event
assertions.

| Area | Coverage | Status |
| --- | --- | --- |
| VM reuse, shutdown, and recreation | New `SimpleTests::VmBackendShutdownAndReconnect`: repeated launches retain the boot ID while kept alive; shutdown produces a different boot ID; stdout, stderr, and nonzero process exit status survive the guest channels. | Added; execution pending. |
| Initialization errors | Existing `UnitTests::KernelModules` checks missing-kernel/module errors. Configuration changes restart the service; this does not establish recovery within the same failed session. | Run against the refactor build. |
| Import/export guest sockets | Existing `UnitTests::ImportExportStdout`. | Run against the refactor build. |
| HCS networking | Existing `NetworkTests::NatDnsTunneling`. | Run against the refactor build. |
| HCS VirtioFS | Existing `DrvFsTests::WSL2VirtioFs::DrvfsMountElevated` and `DrvfsMountNonElevated`. | Run against the refactor build. |
| Guest diagnostics | Existing `UnitTests::DmesgCollection`. | Run against the refactor build; dmesg is not an ETW assertion. |
| Error/lifecycle telemetry | Refactor diff preserves `CreateVmBegin`, `CreateVmEnd`, `FailedToStartVm`, `WslCoreVmInitialize`, `TerminateVmStart`, `TerminateVm`, and `ForceTerminateVm` call sites and payloads. | Source preservation reviewed. These tests do not assert runtime telemetry events, and the default test configuration disables telemetry. |

## CI and runtime sign-off

All repository tests are assumed to run in CI, as confirmed for this workstream.
The execution entries above belong to CI; passing results remain a merge/sign-off
requirement, not an outstanding implementation task for B6.

Runtime sign-off is delegated to CI against the fully built WSL binaries. If
local reproduction is needed, the normal test setup replaces installed WSL and
unregisters/reimports the test distribution; do not run it on a development host
without approval. Fast mode only counts when the new service is deployed and the
test distribution is already prepared.

CI should also exercise the rebased upper layers, since their shared
transport/share-handler changes are not part of the refactor build.

## Local build evidence

The full Release build, including the new test code, completed successfully with:

```powershell
cmake --build build --config Release -- -m /nologo /verbosity:quiet
```

No deployment or runtime test execution was performed.
