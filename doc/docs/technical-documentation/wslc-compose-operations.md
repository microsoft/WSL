# WSLC Compose operations

The internal native COM API is defined in
`src\windows\service\inc\wslc.idl` and implemented by `WSLCSession`,
`WSLCComposeOperation`, and `ComposeReconciler` in `src\windows\wslcsession`.
None of these Compose interfaces is published in the public WSLC SDK.

An operation captures one invocation's inputs, interprets them in the session
process, and executes against the session's Docker resources. Its lifetime and
result belong to that process. There is no durable operation identifier,
reconnection protocol, or persisted execution plan.

## Purpose and contract boundary

An operation captures inputs and exposes progress, completion, cooperative
cancellation, and a final result for one invocation. Document interpretation
precedes resource execution. The reconciler serializes project discovery,
mutation, and final observation.

## Calling the internal API

1. Prepare a `WSLCComposeOperationRequest` using `WSLC_COMPOSE_SCHEMA_VERSION`.
   Select captured documents or an existing normalized project key and supply
   the action's matching options variant.
2. Call `IWSLCSession::BeginComposeOperation` with an optional progress callback.
   The call validates the request envelope, copies the inputs needed by the
   worker, and starts execution. Request buffers can be released when it returns.
   Callback delivery can start before the call returns.
3. Call `IComposeOperation::GetCompletionEvent` and own the returned handle with
   normal handle RAII. It is a `SYNCHRONIZE`-only duplicate of a manual-reset event.
4. Obtain the session's process handle with `IWSLCSession::GetProcessHandle`,
   which includes `SYNCHRONIZE` access. Wait for either completion or process
   exit. To request cancellation, call `Cancel` and continue waiting on both
   handles.
5. After completion, call `GetResult`. Check both the method's HRESULT and
   `WSLCComposeOperationResult::Result`; successful retrieval can return a failed
   or cancelled operation. `ErrorMessage` carries an optional localized failure
   description. Free both `ErrorMessage` and `AffectedContainers` with
   `CoTaskMemFree`, including on repeated result retrieval.

`BeginComposeOperation` reports invalid request envelopes synchronously.
Document interpretation, resource discovery, and execution failures are
reported through the operation result.

The duplicated completion event can remain unsignaled after the session process
exits. Process exit or a failed result-retrieval RPC means the execution outcome
is unknown; it does not establish cancellation or rollback. Callers must not
wait indefinitely on completion alone or automatically retry the operation.

`GetResult` is nonblocking and returns
`HRESULT_FROM_WIN32(ERROR_IO_INCOMPLETE)` before completion. After completion,
each successful retrieval copies the same stored result into caller-owned
storage, allocating fresh message and container storage when present.
The completion event signals after result publication and callback teardown.

The service worker retains both the native operation and its owning session through execution.
Releasing the client's operation interface does not request cancellation.

## Captured inputs and project identity

Document content is the original byte sequence, including any byte-order mark
used by yaml-cpp for encoding detection. The worker parses those captured bytes;
it does not reopen the Compose source file.

`SourcePath` supplies diagnostic context, and its parent directory resolves
relative bind mounts. Callers should supply absolute source paths so resolution
is independent of the session process's working directory.

Each document must also supply a nonempty `BaseDirectory`.

`ProjectDirectory` supplies the default project name. `ExplicitProjectName`
overrides that default. Interpretation normalizes the chosen name and validates
derived resource names before entering the reconciler.

A project key is the exact normalized project name within the owning WSLC
session. Key-backed requests validate that name without normalizing it again.
Different session processes have independent project namespaces.

The supported document input contains exactly one document.

## Actions and execution

| Action | Accepted project input | Behavior |
|---|---|---|
| `Validate` | Documents | Interpret and validate inputs, then return the project key without resource discovery or mutation. |
| `Create` | Documents | Return a nonempty discovered container set; otherwise create the default network and containers. Existing definitions are not compared. |
| `Up` | Documents | Remove discovered containers, replace the default network, create containers, and start them. |
| `Start` | Documents or project key | Start discovered containers, accepting containers that are already running. |
| `Attach` | Documents or project key | Return discovered container identities and hand off running containers' output streams when a callback is supplied. |
| `Stop` | Documents or project key | Stop discovered containers with `SIGTERM` and the supplied per-container timeout, accepting containers that are already stopped. |
| `Remove` | Documents or project key | Remove stopped project containers, retaining running containers. |

`Stop` requires `WSLCComposeActionOptionsTypeStop` and a timeout no greater than
`LONG_MAX`. All other actions require `WSLCComposeActionOptionsTypeNone`.
`Start`, `Attach`, and `Stop` fail with `ERROR_NOT_FOUND` when no containers match.
Stop attempts the remaining containers after a container stop failure and retains
the first such failure and its error details; cancellation or a callback failure
can interrupt the loop.

Execution holds a per-project lock from discovery through final observation.
Lock acquisition polls for cancellation. Interpretation happens before acquiring
that lock, and `Validate` completes without acquiring it.

The lock coordinates operations using this reconciler. Direct Docker operations
have independent synchronization.

Discovery queries running and stopped containers with the project's
`com.microsoft.wslc.compose.project` label. Every match must carry the supported
`com.microsoft.wslc.compose.metadata-version`, a nonempty
`com.microsoft.wslc.compose.service` label, and a positive integer
`com.microsoft.wslc.compose.container-number`. Unsupported metadata, malformed
identities, and duplicate service/container-number pairs fail discovery.
Containers are ordered by service name and numeric container number for execution
and results.

The reconciler does not compare configuration hashes, reconcile orphans, or
provide transactional rollback.
`Up` is destructive, and a failure can leave the project partially or fully
absent.

## Progress and callback threading

Multiple operations can be submitted concurrently. Within one session, operations
on the same project wait for its execution lock; operations on different projects
have independent locks. `Validate` does not acquire a project lock.

The global interface table marshals the callback into the worker's COM apartment.
Native calls are synchronous and serialized on that worker. Native sequence
numbers start at one for each operation. Native callback payloads are borrowed
for the duration of a call.

Callbacks must return promptly and must not wait for operation completion.
Resource callbacks run while the project lock is held, so waiting for another
operation on the same project can deadlock. Clients must service their callback
apartment while waiting for completion.

For example, operation A holds the project lock and calls `OnProgress`. If that
callback starts operation B on the same project and waits for B to complete, B
cannot acquire the lock, and A cannot release it until the callback returns.
Starting B and returning from the callback does not create this wait cycle.
Waiting for A from its own callback also deadlocks. Callbacks should copy or
enqueue progress and return; completion waits belong outside the callback.

An STA client must also dispatch incoming COM calls while waiting. A blocking
wait that prevents callback delivery can stall even a single operation.

Status events describe validation, planning, execution, and the terminal
outcome. These are coarse phase notifications: `Executing` can be delivered
before the worker waits for the project lock.

Resource progress is emitted before the associated work. `Current` is the
resource's one-based ordinal within an operation group, and `Total` is that
group's resource count. A `1/1` event announces work; it does not certify that the
resource operation completed or measure image-download bytes.

A callback failure during active work aborts execution with that HRESULT.
Terminal status and failure diagnostic delivery are best effort; failures are
logged and preserve the execution outcome. Best effort applies to returned
errors, not callback duration. A stalled resource callback holds the project lock and delays
cancellation checkpoints; a stalled terminal callback delays completion. The
native completion event signals after callback teardown and result publication.

Non-cancellation execution failures also produce an `operation_failed` error
diagnostic when a callback is available. Its arguments contain the HRESULT in
hexadecimal and the localized failure message as UTF-8, which can be empty.
The source path is supplied for document-backed requests. Property paths and
line/column locations are not populated. The final result remains authoritative;
the CLI renders this failure through the result so it appears once.

## Stream handoff

After reconciliation releases the project lock, `Up` and `Attach` open output
streams for each running container and call `OnStreamsReady`. Attachment accepts
a container that stops during handoff by skipping its streams. These are
read-only stdout/stderr streams; interactive stdin and TTY are not supplied.
Without a callback, the operation returns container identities without opening
streams.

The callback must duplicate any handles it retains and return promptly. The
incoming handles expire when the callback returns. The CLI retains owned pipe
or socket duplicates and relays all delivered streams concurrently after
operation completion, with its cancellation event in the relay wait set.
Completion waits for stream handoff, not for container output to reach EOF.
Calling `Cancel` on a completed native operation does not cancel client-side
relaying.

The CLI keeps the VM active while relaying. Cancelling standalone attachment
ends relaying; cancelling attached `Up` also requests graceful project stop,
with force-kill escalation on the second interrupt.

## Cancellation and partial state

`IComposeOperation::Cancel` is idempotent and requests cooperative cancellation.
The worker observes it while acquiring the project lock and between resource
operations. It does not interrupt an in-flight Docker or container call, and a
late cancellation request can leave a successful result.

The completion event signals only after work completes, including after a
cancellation request.

Observed cancellation produces `WSLCComposeOperationStatusCancelled` and
`HRESULT_FROM_WIN32(ERROR_CANCELLED)`. A successful late-cancellation outcome
produces `WSLCComposeOperationStatusSucceeded`. Completed mutations remain visible.
During creation, observed cancellation preserves completed, labeled resources
for rediscovery. Other creation failures attempt cleanup of resources created
by that invocation.

`AffectedContainers` is populated from final observation after successful
execution. It is not a mutation journal or an inventory of partial changes after
failure or cancellation. The project key can also be empty if interpretation
failed. Callers must rediscover resources before making recovery decisions.

`ErrorMessage` preserves the worker's localized failure description across the
asynchronous boundary, including when no progress callback is supplied or a
terminal callback fails. It is independent of the caller thread's COM error
information.

## Regression coverage

`test\windows\wslc\WSLCComposeTests.cpp` covers request validation, discovery,
resource ordering, error details and diagnostic delivery, stream handoff,
session-process exit, project concurrency, cancellation checkpoints, late
cancellation, and partial state.
`WSLCE2EComposeLifecycleTests.cpp` covers the CLI lifecycle.

Build the full Windows project before running the focused suite:

```powershell
cmake --build . -- -m
.\bin\x64\debug\test.bat '/name:*Compose*'
```
