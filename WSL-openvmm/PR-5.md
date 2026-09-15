# PR 5 - Backend selection and manual rollback

[Back to the implementation tracker](../WSL-openvmm.md)

**Status:** B3, B4, and B7 implemented under the accepted design and policy.
**Dependencies:** PRs 3 and 4.
**Branch:** `user/damanmmulye/wsl-openvmm-backend`.
**Commit:** `b8b146cf`, after rebasing onto PR 3 commit `b956db18`.
**Source evidence:** S1, S2, and S12 in the main tracker.

## Accepted design and policy

`LxssUserSessionImpl::_CreateVm()` is the centralized creation/selection point;
callers use `IWslCoreVm`. This is accepted instead of a separate factory class.
Shutdown dispatch uses the backend recorded at creation. Scope is WSL, not WSLC.

HCS remains the default, with OpenVMM explicitly opt-in behind the build gate.
Unavailable OpenVMM and initialization failures return errors without automatic
HCS fallback. A live VM retains its selected backend; disabling OpenVMM and
shutting down enables manual rollback on the next creation.

The complete behavior matrix is maintained in the
[service architecture documentation](../doc/docs/technical-documentation/wslservice.exe.md#experimental-vm-backend-selection).
No separate factory or automatic fallback was introduced.

## Committed changes

Commit `b8b146cf` contains the policy documentation and tests on the backend layer:

- `doc\docs\technical-documentation\wslservice.exe.md`: accepted boundary and selection/rollback matrix.
- `test\windows\VmBackendTests.cpp`: configuration parsing and backend-selection integration tests.
- `test\windows\UnitTests.cpp`: invalid backend-boolean warning assertion using the existing helper.
- `test\windows\CMakeLists.txt`: test source registration, matching build-availability definition, and `configfile` linkage.

## Regression coverage

| Test | What it covers |
| --- | --- |
| `VmBackendTests::SelectionConfigParsing` | Absent, false, true, and invalid `experimental.openVmm` values using the real configuration parser. |
| `VmBackendTests::HcsDefaultAndExplicitSelection` | Default and explicit HCS selection; normal/forced shutdown and removal of the matching HCS VM. |
| `VmBackendTests::OpenVmmSelectionAndRollback` | Included builds: OpenVMM/HCS identity, unchanged live VM identity after configuration edits in both directions, normal/forced shutdown, and rollback. Excluded builds: repeated localized `E_NOTIMPL` errors and recovery to HCS. |
| `VmBackendTests::OpenVmmCreationFailureDoesNotFallback` | Included builds: rejection of an unsupported `.img` system-distro configuration, repeated errors, successful OpenVMM retry and HCS rollback without restarting the service. This is an early failure, not a post-allocation fault-injection test. |
| Existing `UnitTests` configuration-warning case | Invalid `experimental.openVmm` value produces the expected warning. |

Identity assertions correlate `wslinfo --vm-id` with HCS lookup and the OpenVMM RPC
endpoint rather than merely checking that a guest command succeeds. Shutdown
assertions check that the corresponding HCS VM and RPC directory are absent.

## CI requirements and limitations

Run against matching service/test binaries with `INCLUDE_OPENVMM=OFF` and `ON`;
running all tests in one configuration does not cover both availability paths.
Enabled runs require the packaged OpenVMM artifacts and a compatible host.

Runtime execution is delegated to CI. Results remain a merge/sign-off requirement;
the implemented status does not claim passing results or exhaustive fault
injection. Tests do not inject failures at every resource-initialization stage.

## Local build evidence

The OpenVMM-enabled `wsltests` target built and linked successfully. The
excluded-backend test branch also passed a compiler syntax check using the
existing build flags.

No local deployment or runtime execution was performed.
