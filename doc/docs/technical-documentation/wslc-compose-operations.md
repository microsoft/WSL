# WSLC Compose operations

The internal native COM API is defined in
`src\windows\service\inc\wslc.idl` and implemented by `WSLCSession`,
`WSLCComposeOperation`, and `ComposeReconciler` in `src\windows\wslcsession`.
None of these Compose interfaces is published in the public WSLC SDK.

An invocation captures its inputs, interprets them in the session process, and
executes against the session's Docker resources. The native call remains pending
through execution, callback delivery, and cleanup. There is no durable operation identifier,
reconnection protocol, or persisted execution plan.

## Purpose and contract boundary

The synchronous native API accepts inputs, an optional progress callback, and an
optional cancellation event, then returns a final result. Document interpretation
precedes resource execution. The reconciler serializes project discovery,
mutation, and final observation.

## Calling the internal API

1. Prepare a `WSLCComposeOperationRequest` using `WSLC_COMPOSE_SCHEMA_VERSION`.
   Select captured documents or an existing normalized project key and supply
   the action's matching options variant.
2. Call `IWSLCSession::ExecuteComposeOperation` with the request, an optional
   progress callback, an optional manual-reset cancellation event, and result
   storage. Keep the request buffers valid until the method returns.
3. If the caller must remain responsive, make the call on a COM-initialized
   background thread. Marshal COM interfaces when crossing apartments.
   The input or I/O coordinator can signal the cancellation event independently;
   it must keep the call's resources alive until the call returns.
4. Check the method's HRESULT first. On `S_OK`, check
   `WSLCComposeOperationResult::Result`, which carries the execution HRESULT.
   `ErrorMessage` carries an optional localized failure description. Free both
   `ErrorMessage` and `AffectedContainers` with `CoTaskMemFree`.

Invalid request envelopes fail the method. Interpretation, discovery, execution,
and observed cancellation are reported in the terminal result. A transport
failure or service-process exit means the execution outcome is unknown; it
does not establish cancellation or rollback, and must not trigger an automatic
retry.

The executor is call-scoped. There is no remote operation object, separate
completion event, or result-retrieval RPC. Returning successfully ends callback
delivery for that invocation.

`SchemaVersion` identifies the accepted internal payload revision. The CLI and
service ship in lockstep; this field does not negotiate independently deployed
versions or make incompatible COM layout changes safe. Persisted resource
metadata has its own version and validation.

## Captured inputs and project identity

Document content is the original byte sequence, including any byte-order mark
used by yaml-cpp for encoding detection. The service parses those captured bytes;
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
| `Attach` | Documents or project key | Return discovered container identities. The operation does not establish terminal streams. |
| `Stop` | Documents or project key | Stop discovered containers with their configured default signal and the supplied per-container timeout, accepting containers that are already stopped. |

`Stop` requires `WSLCComposeActionOptionsTypeStop` and a timeout no greater than
`LONG_MAX`. All other actions require `WSLCComposeActionOptionsTypeNone`.
`Start`, `Attach`, and `Stop` fail with `ERROR_NOT_FOUND` when no containers match.
Stop attempts the remaining containers after a container stop failure and retains
the first such failure and its error details; cancellation or a callback failure
can interrupt the loop.

Execution holds a per-project lock from discovery through final observation.
Lock acquisition waits on the project mutex, caller cancellation, session
termination, and caller-process exit. Abandoned mutexes fail execution, and
recursive acquisition rejects nested project execution. Interpretation happens
before acquiring that lock, and `Validate` completes without acquiring it.

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

Discovery and public container listing share typed Docker enumeration. Discovery
also validates raw labels and recovers uncached containers; the public listing
formats labels for display and omits uncached containers. Recovery rechecks
existence and state after subscribing to container events. A container deleted
after enumeration is not published as a recovered container.

Results contain the container's image reference, matching public container
listing. Read-only wrapper properties remain accessible after deletion, so a
successful Stop of an auto-remove container reports its `Deleted` state.

The reconciler does not compare configuration hashes, reconcile orphans, or
provide transactional rollback.
`Up` is destructive, and a failure can leave the project partially or fully
absent.

## Progress and callback threading

Multiple operations can be submitted concurrently. Within one session, operations
on the same project wait for its execution lock; operations on different projects
have independent locks. `Validate` does not acquire a project lock.

Callbacks are synchronous and serialized on the incoming execution thread. Sequence
numbers start at one for each operation. Native callback payloads are borrowed
for the duration of a call.

Callbacks must return promptly and must not wait for operation completion.
Resource callbacks run while the project lock is held, so waiting for another
operation on the same project can deadlock. Clients must service their callback
apartment while the native call is pending.

For example, operation A holds the project lock and calls `OnProgress`. If that
callback starts operation B on the same project and waits for B to complete, B
cannot acquire the lock, and A cannot release it until the callback returns.
Starting B and returning from the callback does not create this wait cycle.
Waiting for A from its own callback also deadlocks. Callbacks should copy or
enqueue progress and return; completion waits belong outside the callback.

An STA client must also dispatch incoming COM calls while waiting. A blocking
wait that prevents callback delivery can stall even a single operation.

Status events describe validation, execution, and the terminal outcome.
`Executing` is a coarse phase notification delivered before the project-lock
wait; it is not evidence that mutation has started. This prototype does not
construct a reconciliation plan or emit a planning phase.

Resource progress is emitted before the associated work. `Current` is the
resource's one-based ordinal within an operation group, and `Total` is that
group's resource count. A `1/1` event announces work; it does not certify that the
resource operation completed or measure image-download bytes.

A callback failure during active work aborts execution with that HRESULT.
Terminal status delivery is best effort; its failure is logged and preserves
the execution outcome. Best effort applies to returned errors, not callback
duration. A stalled resource callback holds the project lock and delays
cancellation checkpoints; a stalled terminal callback delays completion. The
native call returns after callback delivery and result construction.

The engine emits status and resource-progress events. `Attach` returns a
container snapshot for caller-side attachment.

## Cancellation and partial state

Signaling the caller's manual-reset event requests cooperative cancellation.
The service observes it while acquiring the project lock and between resource
operations. It does not interrupt an in-flight Docker or container call, and a
late cancellation request can leave a successful result.

Session termination and caller-process exit are also cancellation sources.
Session shutdown prevents new callback registrations and repeatedly cancels
outgoing callbacks until registered callbacks and Compose invocations drain.
This drain is independent of obtaining the runtime lock: an invocation can be
between callbacks or hold no VM lease. User cancellation alone does not interrupt
a stalled progress callback; callbacks must return promptly.

Cancellation is a request, not completion. The native method remains pending
until its work and cleanup finish.

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

`ErrorMessage` preserves the service's localized failure description in the
result, including when no progress callback is supplied or a
terminal callback fails. It is independent of the caller thread's COM error
information.

## Interactive callers and future SDK projection

The service owns command policy and its terminal outcome regardless of the
client's threading model. A future attached-command implementation can keep the
native call pending while handing off streams incrementally. The client must
consume those streams concurrently with execution, using a synchronized handoff
to its I/O coordinator. A stream callback must return promptly and must not run
the relay itself. Service completion and client output draining are separate
milestones, and project mutation locks must not span indefinite attachment.

This prototype's `Attach` only returns container identities. It does not implement
full attached-command lifetime, force-cancellation escalation, or parallel
resource scheduling. These capabilities are independent of whether the native
entry point is synchronous. A future public SDK can project the call through
standard WinRT async interfaces with explicit cancellation bridging.

## Regression coverage

`test\windows\wslc\WSLCComposeTests.cpp` covers request validation, discovery,
resource ordering, error details, session-process exit, project concurrency,
cancellation checkpoints, late cancellation, partial state, callback shutdown,
deletion during Up, and auto-remove Stop.
`WSLCE2EComposeLifecycleTests.cpp` covers the CLI lifecycle.

Build the full Windows project before running the focused suite:

```powershell
cmake --build . -- -m
.\bin\x64\debug\test.bat '/name:*Compose*'
```
