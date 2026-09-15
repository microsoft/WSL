# WSL OpenVMM RPC contract

This private DLL implements VMService **gRPC over Windows AF_UNIX** using Tonic.
It does not implement ttrpc. The service schema is supplied by `VM_SERVICE_PROTO`
at build time. C++ callers use `wslopenvmm.h`; handles remain Rust-owned.

## Layer boundaries

`lib.rs` validates pointers, converts strings, manages opaque-handle ownership,
and delegates to the client. It contains no locking or VM lifecycle policy.

`client.rs` owns the handle operations, configuration builder, synchronization,
request construction, timeout budgets, and recovery policy. Its VM mutex protects the runtime, Tonic client,
share map, and VM status together. Only the cancellation signal lives outside that
mutex so cancellation does not wait behind an RPC. One operation
entry path handles timed locking, invalidation checks, and cleanup exceptions.
Deadlines are passed explicitly, never stored as mutable handle state.
`VmConfigHandle` similarly owns configuration locking and the creation budget.
Its `builder` field holds the synchronized builder, whose `config` field holds
the protobuf configuration. `inner` is reserved for `VmHandle`'s hidden, locked
implementation.

VM status is `Active` or `RecoveryRequired`, stored as an ordinary enum under the
per-handle mutex. There is no separate atomic VM status. Builder status is `Ready` or
`CreationOutcomeUnknown`, protected by the configuration lock. Both transitions
are permanent for that handle.

`rpc.rs` executes futures against a deadline and optional cancellation receiver,
then classifies and translates failures. It does not know about VM state or
decide whether to invalidate a handle; that decision remains in the client.
Only the DLL exports and opaque handle types are public. Internal modules are
private; their intended interfaces use `pub` for access across modules, and
implementation-only constructors and error-mapping helpers remain private.
`client/tests.rs` is compiled as a child of the client module so it can inspect
private state without exposing that state to the DLL layer.

## Deadlines and ownership

`WslOpenVmmCreateVm` requires a nonzero `TimeoutMs`. One deadline covers waiting
for the configuration lock, connecting, and receiving the CreateVm response.
Missing or refusing startup endpoints are retried every 20 ms within that budget.
Invalid paths and other connection failures return immediately. HTTP/2 connection
establishment and RPC completion are bounded locally, even if the peer ignores
the `grpc-timeout` metadata.

Every subsequent VM operation gets the configured timeout, including time waiting
for the per-VM mutex. The remaining budget is also sent as `grpc-timeout`.
A lock timeout before dispatch does not itself invalidate a VM.

Successful creation consumes the configuration and returns a VM handle. Failure
leaves the configuration owned by the caller and returns a null VM handle.
An uncertain CreateVm outcome permanently disables further creation with that
configuration: the remote VM might exist despite the missing success response.
Dispose of that configuration and its owning OpenVMM process before recreating
both. A connection failure before dispatch or a definite rejection can be retried.

Destroy each handle exactly once. Destruction must not overlap any call using
that handle; successful CreateVm consumption must not overlap configuration use.
DestroyVm frees the local handle only; it does not guarantee remote teardown.

## Fail-closed recovery

Requests are never replayed by this wrapper. A deadline, cancellation, transport
failure, or other uncertain RPC outcome permanently invalidates further VM
mutations. For example, a share add may have succeeded remotely while its response
was lost. The local share map changes only after a successful response; it must
not be treated as authoritative after invalidation.

Invalidation is internal client state. Later mutations on an invalidated handle return
`HRESULT_FROM_WIN32(ERROR_INVALID_STATE)` without sending another request.
Teardown and quit remain available as bounded, best-effort cleanup, but neither
clears invalidation. Recovery requires a fresh process, configuration, and handle,
not replay, transparent VM reconnection, or remote resource reconciliation.
Tonic's internal channel reconnection does not establish VM/resource continuity.

`WslOpenVmmVmCancelRequests` is nonblocking and may run concurrently with an RPC.
It sets a permanent cancellation signal and interrupts an active mutation with
`E_ABORT`. The operation path records `RecoveryRequired` under the mutex when
observing cancellation, before dispatch or after completion (including a successful
response racing cancellation). Cancellation does not prove the server stopped executing the request.
Cleanup ignores this cancellation signal but still observes its own deadline.
A queued caller remains bounded by its original lock deadline. Status is checked
after acquiring the mutex; lock contention can therefore return WAIT_TIMEOUT even
when the handle already requires recovery.

The process owner must keep the handle alive during cancellation, quiesce users
before destruction, and discard the process after an uncertain creation. Process
exit/forced-termination callbacks should cancel outstanding requests and route
the VM through the existing shutdown/recreation path. Those service-side hooks
belong in the dependent backend branch, not in this RPC-only layer.

## Error mapping

| gRPC result | HRESULT | Invalidates mutations |
| --- | --- | --- |
| InvalidArgument, OutOfRange | `E_INVALIDARG` | No |
| NotFound | `HRESULT_FROM_WIN32(ERROR_NOT_FOUND)` | No |
| AlreadyExists | `HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)` | No |
| PermissionDenied | `E_ACCESSDENIED` | No |
| Unauthenticated | `HRESULT_FROM_WIN32(ERROR_LOGON_FAILURE)` | No |
| FailedPrecondition | `HRESULT_FROM_WIN32(ERROR_INVALID_STATE)` | No |
| Unimplemented | `E_NOTIMPL` | No |
| Cancelled | `E_ABORT` | Yes |
| DeadlineExceeded, local deadline | `HRESULT_FROM_WIN32(WAIT_TIMEOUT)` | Yes |
| Unavailable | `HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED)` | Yes |
| Unknown, Internal | `E_FAIL` | Yes |
| ResourceExhausted | `HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY)` | Yes |
| Aborted | `HRESULT_FROM_WIN32(ERROR_RETRY)` | Yes, despite the error name |
| DataLoss | `HRESULT_FROM_WIN32(ERROR_INVALID_DATA)` | Yes |

The non-invalidating statuses are definite validation/authorization rejections:
the server must not report these after partially applying a mutation. No retry is
automatic, including for these statuses. A FailedPrecondition rejection and a
locally invalidated handle share an HRESULT; no state-query API is exposed.
Tonic's typed local timeout error is mapped to WAIT_TIMEOUT, not E_ABORT.
Connection I/O errors preserve native OS codes where available, with explicit
timeout, access-denied, and invalid-input categories; classification does not
depend on matching error-message text.

## RPC tracing

The DLL follows Hvlite epci2's `ensure_tracing_init` pattern: `Once` initializes
a global `tracing_subscriber::Registry` with a debugger-filtered `fmt` layer.
Configuration creation initializes tracing outside the loader lock. Debug-level
and higher messages go to `OutputDebugStringA`, without ANSI escapes; formatting
is skipped when no debugger is attached. The filter accepts only the
`wslopenvmm::rpc` target, excluding dependency traces that could expose payloads.

`DebugOutputWriter::new` is the writer factory. Following OpenVMM's
`support/debug_output_tracing`, it appends a NUL terminator using a
`SmallVec<[u8; 1024]>`, keeping typical messages on the stack and allocating only
for longer output.

Tracing uses ordinary `tracing::info!`, `tracing::warn!`, and `tracing::error!`
messages at the client and RPC call sites. Messages describe operation start and
completion, elapsed time, HRESULTs, gRPC status codes, lock/connection/dispatch
failures, cancellation, and recovery state. They distinguish pre-dispatch failures
from uncertain RPC outcomes and preserve the distinction between successful
cleanup and a usable VM.

There is no custom structured event schema, diagnostic context/operation wrapper,
ID generator, or correlation metadata on requests. Logs rely on message text and
the formatter's timestamps and debugger context. Paths, command
lines, share tags, request payloads, and server-provided error messages are not logged.

There is no ETW provider, reload layer, or custom `DllMain` tracing hook. These
messages are debugger output, not part of the standard WSL WPR trace collection.
Handles contain no tracing state, and macros use the global subscriber directly.
Destroy all handles and quiesce all DLL calls/workers before unloading the DLL.
Failure to install the global subscriber is reported through debug output without
replacing an existing subscriber.
Service/process diagnostics and the backend-wide failure taxonomy remain backend work.

## Tests

Run `cargo test` for this crate on Windows with `VM_SERVICE_PROTO` pointing to
the compatible VMService schema. Tests use isolated AF_UNIX endpoints and mock
servers; they neither launch OpenVMM nor deploy WSL. They cover ABI serialization,
deadline budgets, cancellation, invalidation, creation ownership, cleanup,
startup retries, missing endpoints, invalid paths, and silent/wrong-protocol peers.
Real OpenVMM process crashes, lifecycle integration, service-wide correlation,
and end-to-end recovery remain separate backend validation work.
