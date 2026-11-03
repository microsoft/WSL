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

1. Prepare a `WSLCComposeOperationRequest`.
   Select a captured `WSLCComposeDocumentCollection` or an existing normalized project key and supply
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

The CLI and service ship in lockstep. Changes to these internal COM layouts
require updating both sides together; there is no payload-version negotiation.
Persisted resource metadata has its own version and validation.

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
| `Attach` | Documents or project key | Return discovered container identities and hand off running containers' output streams when a callback is supplied. |
| `Stop` | Documents or project key | Stop discovered containers with their configured default signal and the supplied per-container timeout, accepting containers that are already stopped. |
| `Remove` | Documents or project key | Remove stopped project containers, retaining running containers. |

`Stop` requires `WSLCComposeActionOptionsTypeStop` and a timeout no greater than
`LONG_MAX`. All other actions require `WSLCComposeActionOptionsTypeNone`.
`Start`, `Attach`, and `Stop` fail with `ERROR_NOT_FOUND` when no containers match.
Stop attempts the remaining containers after a container stop failure and retains
the first such failure and its error details; cancellation can interrupt the loop.
Progress observer failures do not interrupt execution.

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

Every `WSLCComposeProgressEvent` carries the whole invocation's current `Status`
and optional `Progress` and `Diagnostic` payloads. `Status` describes the invocation's phase or
outcome. `Progress` provides granular detail where applicable, such as creating
a network or starting a container, and is null when no granular details apply.
Multiple progress notifications can carry the same status without reporting a
phase transition. All notifications are advisory; the synchronous result is
authoritative.

For example, creating a new one-container project emits:

| Status | Progress |
|---|---|
| `Validating` | Null |
| `Executing` | Null |
| `Executing` | Creating the default network, ordinal 1 of 1 |
| `Executing` | Creating the container, ordinal 1 of 1 |
| `Succeeded` | Null |

The initial `Executing` notification is delivered before the project-lock
wait; it is not evidence that mutation has started. The engine does not
construct a reconciliation plan or emit a planning phase.

Resource progress is emitted before the associated work. `Current` is the
resource's one-based ordinal within an operation group, and `Total` is that
group's resource count. A `1/1` event announces work; it does not certify that the
resource operation completed or measure image-download bytes.

`OnProgress` HRESULT failures and caught callback exceptions are logged and do not
abort execution or replace its outcome. Clients request cancellation through the
cancellation event, not through the callback's return value. Best effort applies to returned errors, not callback
duration. A stalled resource callback holds the project lock and delays
cancellation checkpoints; a stalled terminal callback delays completion. The
native call returns after callback delivery and result construction.

Non-cancellation execution failures include an `operation_failed` error
diagnostic in the `Failed` notification when a callback is available.
Its arguments contain the HRESULT in
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

Stream-handoff failures fail the operation. The callback must duplicate any handles
it retains and return promptly. The
incoming handles expire when the callback returns. The CLI retains owned pipe
or socket duplicates and relays all delivered streams concurrently after
operation completion, with its cancellation event in the relay wait set.
Completion waits for stream handoff, not for container output to reach EOF.
Signaling the native cancellation event after the call returns does not cancel
client-side relaying.

The CLI keeps the VM active while relaying. Cancelling standalone attachment
ends relaying; cancelling attached `Up` also requests graceful project stop,
with force-kill escalation on the second interrupt.

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

The CLI runs native execution on a COM-initialized thread while its coordinator
waits on local completion and interrupt events. It forwards cancellation through
a manual-reset event and can force-kill project containers during a pending
graceful Stop. It joins the execution thread before releasing request, result,
or callback storage. The completion event is client-local; native RPC failure
also completes that worker.

The service owns command policy and its terminal outcome regardless of the
client's threading model. A future attached-command implementation can keep the
native call pending while handing off streams incrementally. The client must
consume those streams concurrently with execution, using a synchronized handoff
to its I/O coordinator. A stream callback must return promptly and must not run
the relay itself. Service completion and client output draining are separate
milestones, and project mutation locks must not span indefinite attachment.

The operation returns after handing off output streams. Full service-owned
attached-command lifetime and parallel resource scheduling are not implemented.
These capabilities are independent of whether the native entry point is
synchronous. A future public SDK can project the call through standard WinRT
async interfaces with explicit cancellation bridging.

## Regression coverage

`test\windows\wslc\WSLCComposeTests.cpp` covers request validation, discovery,
resource ordering, error details and diagnostic delivery, stream handoff,
session-process exit, project concurrency, cancellation checkpoints, late
cancellation, partial state, nonfatal observer failures, callback shutdown, deletion during Up, and
auto-remove Stop.
`WSLCE2EComposeLifecycleTests.cpp` covers the CLI lifecycle.

Build the full Windows project before running the focused suite:

```powershell
cmake --build . -- -m
.\bin\x64\debug\test.bat '/name:*Compose*'
```
