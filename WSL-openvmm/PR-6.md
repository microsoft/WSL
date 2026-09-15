# PR 6 - OpenVMM process supervision and gRPC client

[Back to the implementation tracker](../WSL-openvmm.md)

**Status:** C1 implemented; C2 and C7 closed for the accepted RPC-layer and logging
scopes. Backend integration remains separately tracked follow-up work. G4 remains
partial.
**Branch:** `user/damanmulye/wsl-openvmm-rpc`.
**Changes:** C2 follow-up committed as `3e729f2d`, propagated to the backend branch,
and pushed. C7 RPC diagnostics are a subsequent uncommitted follow-up.
**Source evidence:** S4, S5, S9, S10, S13, and S14 in the main tracker.

## Task checklist

- [x] **C1:** Existing backend process launch, supervision, termination, and cleanup.
- [x] **C2, contract:** Accept gRPC/AF_UNIX and fail-closed recovery instead of ttrpc and transparent reconciliation.
- [x] **C2, deadlines/errors:** Bound connection and RPC waits, share the lock/I/O deadline budget, and preserve distinct HRESULT categories.
- [x] **C2, recovery:** Keep VM/configuration status enums sticky after uncertain outcomes; reject further mutations without replay; retain bounded cleanup.
- [x] **C2, ownership:** Keep locking, status, and request policy in the client; leave DLL exports as ABI adapters and RPC helpers independent of VM state.
- [x] **C2, synchronization:** Use one mutex per handle for mutable state, with only the cancellation signal outside the VM mutex; remove atomic status.
- [x] **C2, simplification:** Remove the unused recreation-query API and forwarding-file split; retain private modules with plain `pub` interfaces and private implementation helpers.
- [ ] **Backend lifecycle follow-up (outside C2 closeout):** Wire cancellation into process exit/termination, coordinate handle lifetime, and exercise teardown followed by fresh-process recovery.
- [x] **C7, RPC errors:** Distinguish timeout, cancellation, validation, authorization, transport, and other server failures.
- [x] **C7, RPC diagnostics (accepted logging approach):** Emit ordinary tracing macros for operation progress, elapsed time, HRESULT/gRPC errors, cancellation, and recovery state through an epci2-style, once-initialized debugger subscriber. No ETW provider, structured event schema, or request correlation metadata is required.
- [ ] **Backend diagnostics follow-up (outside C7 closeout):** Connect service/process activities and define the backend-wide failure taxonomy.
- [x] **G4, client/transport slice:** Cover AF_UNIX startup, bounded waits, cancellation, status transitions, share serialization, and invalid/silent/wrong-protocol endpoints.
- [ ] **G4, service/process slice:** Cover real process death, post-allocation failures, lost-response resource state, and service shutdown races.

These entries break down the existing scope bullets; they do not add new bullets
to the main tracker's totals. C2 and C7 are closed for the accepted scopes;
their retained backend follow-ups are not implemented by that closeout. G4 remains partial.

## Accepted C2 contract

Keep gRPC over AF_UNIX instead of changing the implementation to ttrpc. Replace
transparent runtime reconciliation with fail-closed recovery: never replay an
uncertain mutation, reject subsequent mutations on the same handle, and require
teardown/process recreation. This is an accepted scope revision, not a claim
that remote-state reconciliation was implemented.

The maintained [RPC contract](../src/windows/wslopenvmm/README.md) documents
ownership, deadlines, cancellation, error mapping, and process-owner obligations.

## RPC-layer changes

Connection establishment has a local deadline and typed I/O error propagation.
CreateVm and subsequent operations have local response deadlines in addition to
gRPC timeout metadata. Each call shares one budget across mutex waiting and I/O.
The wrapper does not replay operations.

Uncertain results invalidate the VM; share bookkeeping changes only on success.
Teardown/quit remain bounded and available after invalidation, without restoring
mutation eligibility. An uncertain CreateVm result also invalidates its retained
configuration, preventing a blind creation retry.

The FFI adds nonblocking cancellation, which can interrupt an in-flight mutation
without acquiring its mutex and must not race handle destruction. Invalidation
remains internal client state; no state-query API is exposed.

The client now owns all locking and lifecycle policy: `VmHandle` exposes shared
`&self` operations, protects VM status with its per-handle mutex, and passes deadlines
explicitly. Configuration locking and creation budgets live in `VmConfigHandle`.
DLL exports only adapt the ABI; RPC helpers accept cancellation receivers and
return classified failures without accessing VM state.

Status enums distinguish VM `Active`/`RecoveryRequired` and configuration
`Ready`/`CreationOutcomeUnknown`, preserving mutex-protected VM status and sticky
failure behavior. Modules remain private, their interfaces use plain `pub`, and
implementation-only helpers and fields remain private; no `pub(super)` remains.
Configuration members are named `builder` and `config`; `VmHandle.inner` remains
the hidden, locked implementation.

Only the cancellation signal remains outside the per-handle mutex. Atomic status
and the separate `VmState` object are removed. Cancellation is observed under the
mutex before dispatch and after completion, including when it races a successful
response. Queued calls still use their original lock deadline.
Status checks occur after acquiring the mutex, so contention may return
WAIT_TIMEOUT even for a handle that already requires recovery. Cancellation is
latched immediately; the enum is updated when the operation path observes the
signal while holding the mutex.

`client.rs` contains the handle operations, builder, lifecycle state,
synchronization, and RPC implementation without a forwarding layer.
The temporary `implementation.rs` split and unused
`WslOpenVmmVmRequiresRecreation` export were removed.
Tests remain in `client/tests.rs`, compiled beneath the client module to
retain private-state access.

## Regression coverage

All VM mock tests now use the production AF_UNIX connector rather than TCP.

| Test or group | What it proves |
| --- | --- |
| Existing port/share ABI tests | Protocol/address-family and share fields remain correct; definite rejection permits an explicit retry. |
| Uncertain share mutation | All ambiguous status categories invalidate; the map is unchanged, later mutations are not sent, and cleanup cannot revalidate the handle. |
| Definite rejections and HRESULT table | Validation/authorization categories remain distinguishable and leave the handle usable. |
| Hung mutation/cleanup; local pending future | Local deadlines bound calls independently of peer timeout support, including cleanup. |
| Lock timeout; combined lock/RPC budget | Expiring before dispatch leaves the VM usable; lock waiting does not restart the RPC budget. |
| Cancellation before/during dispatch and racing success | No mutation after pre-cancellation; active calls are interrupted; cancellation racing success still requires recovery, and cleanup remains available. |
| Creation ownership and timeout | Success consumes the configuration; failures preserve ownership; uncertain creation cannot be replayed. |
| Missing/delayed AF_UNIX endpoint; invalid path | Startup retries are bounded; pre-dispatch connection failure is retryable; malformed paths are not misclassified as timeouts. |
| Silent/wrong-protocol peers | A peer accepting the socket but not speaking gRPC cannot hang creation indefinitely or produce success. |
| Direct-client boundary regressions | Concurrent share additions serialize bookkeeping; cancellation, cleanup, and configuration lock deadlines work without DLL policy; RPC execution accepts a standalone cancellation receiver. |

These are deterministic mock/transport regressions, not proof that OpenVMM's
resource operations are transactional or that every process-failure race is
covered.

## Remaining work

On the dependent backend branch, wire cancellation into
process-exit and termination handling with correct RPC-handle lifetime ordering.
Exercise teardown and fresh-process recovery through actual service call sites.
This remains backend follow-up work outside the accepted C2 RPC-layer closeout.

C7 RPC diagnostics are implemented in `src/windows/wslopenvmm/src/diagnostics.rs`.
Client and RPC code emit ordinary tracing macros with readable messages. A global
`tracing` subscriber uses debugger-filtered formatted output, initialized once
at configuration creation, matching Hvlite epci2's `ensure_tracing_init`.
The writer follows `support/debug_output_tracing` with a stack-backed,
NUL-terminated buffer. There is no ETW provider, reload layer, or tracing
`DllMain` hook. Handles carry no tracing state; diagnostic context
and operation wrappers, generated IDs, and request correlation metadata were
removed in favor of the accepted macro-based approach. Messages distinguish
lock/validation/connection/RPC failures, definite rejections, uncertain outcomes,
and cleanup of an already-invalidated handle without logging payloads or server
error text. Existing tests are retained; no tracing-specific tests were added.
These messages are debugger output, not part of the base WSL WPR profile.

Service/process diagnostics and the backend-wide failure taxonomy remain backend
follow-ups outside the accepted C7 logging closeout.
G4 still needs real process death, post-allocation failures, actual lost-response
and resource-state scenarios, shutdown races, and broader protocol corruption.

## Local evidence and CI

The crate's 22 Rust tests pass on Windows, and
`cargo clippy --all-targets -- -D warnings` passes using the existing compatible
schema. The release RPC DLL also builds with the locked dependencies.
No WSL deployment or real-VM integration tests were run. CI should run this
crate's tests with the matching schema; service integration results remain separate.
