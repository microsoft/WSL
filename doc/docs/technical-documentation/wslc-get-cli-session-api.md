# WslcGetCLISession API Design

## Overview

This document describes the design of `WslcGetCLISession`, a public API in the WSLC (WSL Containers) library that returns a reference to the active CLI session for the current process. This is primarily used during the **inner-loop development experience** — build, run, and debug flows for Windows applications that use Linux containers via WSLC.

### Motivation

The WSLC architecture follows an **app-owns-lifecycle** model:

```
App → Library → Session → Container
```

During the inner-loop development flow, the WSLC toolchain (MSBuild targets, `wslc` CLI, or IDE integration) creates a `WslcSession` to manage container operations. Application code running in the same process — such as build tasks, debug launch helpers, or the app itself during F5 — needs access to this session to interact with the container (e.g., attach a debugger, inspect state, or run additional commands).

`WslcGetCLISession` provides a stable, public mechanism to retrieve the session that the WSLC toolchain has established for the current process.

### Scope

- **In scope**: Retrieving the WSLC CLI session from any code running in the same process where the WSLC toolchain has published a session. This includes the `wslc` CLI process, MSBuild task host processes, Windows app processes launched with WSLC integration, and IDE extension hosts.
- **Out of scope**: Cross-process session sharing, remote session access, session creation.
- **Usage context**: Inner-loop developer experience — build, run, debug. Not a primary production API, but designed to the same quality standards as all WSLC public APIs.

## API Design

### Prerequisites: Existing WSLC Types

The following types are established in the WSLC public API surface and are referenced by this design:

```c
// Opaque session handle (ref-counted)
typedef struct WslcSession_s* WslcSession;

// Standard WSLC lifecycle APIs (already exist)
STDAPI WslcCreateSession(_In_ const WslcSessionConfig* config, _Out_ WslcSession* session);
STDAPI WslcCloseSession(_In_ WslcSession session);

// Increment session reference count (already exists)
void WslcSessionAddRef(_In_ WslcSession session);
```

> **Note**: WSLC uses `Close` instead of `Free` (contrast with `FreeWslConfig` in the WSL Config API) to emphasize ref-counted release semantics — `WslcCloseSession` decrements the reference count and only destroys the session when it reaches zero.

### New API

```c
// ---------------------------------------------------------------------------
// WslcGetCLISession
// ---------------------------------------------------------------------------
//
// Retrieves the active WSLC CLI session for the current process.
//
// The WSLC toolchain (wslc CLI, MSBuild targets, or IDE integration)
// publishes a session during the build/run/debug flow. This API returns
// that session to any code running in the same process — including the
// application being developed.
//
// The returned session handle is ref-counted. The caller receives an owned
// reference and MUST call WslcCloseSession() when finished. Closing the
// returned handle only releases the caller's reference — it does NOT close
// or destroy the underlying CLI session. The toolchain holds its own
// independent reference.
//
// If the process exits or is terminated, all in-process ref counts are
// reclaimed by the OS. There is no leak concern.
//
// Thread safety:
//   - This function is safe to call concurrently from multiple threads.
//   - The returned WslcSession handle is safe for concurrent use across
//     threads (all WslcSession operations are internally synchronized).
//
// Lifetime:
//   - Behavior is undefined if called during CRT static destruction
//     (DLL_PROCESS_DETACH). Callers should release their session handles
//     before process teardown begins.
//
// Parameters:
//   session - [out] Receives the CLI session handle. On failure, set to NULL.
//
// Return values:
//   S_OK                   - Session retrieved successfully.
//   WSLC_E_NO_CLI_SESSION  - No CLI session has been published in the
//                            current process (toolchain not initialized).
//   E_POINTER              - The session parameter is NULL.
//
STDAPI WslcGetCLISession(_Out_ WslcSession* session);
```

> **Export**: `WslcGetCLISession` is exported from the WSLC library DLL via its module `.def` file. Consumers link against the WSLC import library or call `GetProcAddress` on the loaded DLL. The declaration lives in the WSLC public header (`wslc.h`).

### Error Code Definition

```c
// WSLC-specific error codes use FACILITY_ITF (standard for interface-specific errors)
#define WSLC_E_NO_CLI_SESSION  MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x8100)
```

`FACILITY_ITF` is the standard COM facility for interface-specific error codes. The CODE offset `0x8100` is well-separated from the existing `WSL_E_*` error codes, which use CODE offsets in the `0x0300–0x03xx` range within `FACILITY_ITF`. Both produce HRESULTs in the `0x8004xxxx` range, but the CODE field separation avoids collisions.

## Design Decisions

### 1. HRESULT Return Type

**Decision**: Use `HRESULT`, not a custom `WslcResult` enum.

**Rationale**: Every existing WSL public API uses `HRESULT` — the Plugin API, the Config API, the COM service interface, and internal helpers. Introducing a separate error type would:

- Fragment the error handling surface
- Require conversion at every WSL/WSLC boundary
- Lose compatibility with standard Windows tooling (`SUCCEEDED()`, `FAILED()`, `FormatMessage()`)

Custom WSLC-specific error conditions are expressed as custom `HRESULT` values (e.g., `WSLC_E_NO_CLI_SESSION`), which is the standard Windows pattern.

### 2. Ref-Counted Owned Handle

**Decision**: `WslcGetCLISession` returns an **owned, ref-counted** handle. The caller must call `WslcCloseSession()` when finished.

**Rationale**: Returning a borrowed (non-owning) handle would be simpler but introduces safety risks:

| Concern | Borrowed Handle | Owned Handle |
|---------|----------------|--------------|
| Caller accidentally closes it | Use-after-free for CLI | Safe — only releases caller's ref |
| Toolchain tears down during async work | Dangling pointer | Caller's ref keeps session alive |
| Type confusion with owned handles | Same type, different rules | Same type, same rules |
| API surface complexity | Lower | Slightly higher (caller must close) |

Since `WslcSession` is already a ref-counted type in the WSLC API, returning an owned handle is consistent. The minor overhead of an `AddRef` call is negligible compared to the safety benefits.

> **Important**: Calling `WslcCloseSession` on a handle returned by `WslcGetCLISession` **never** closes the actual CLI session. It only decrements the caller's reference. The CLI session continues to operate normally. When the process exits or is terminated, the OS reclaims all in-process memory — including any ref counts — so there is no leak concern regardless of whether the caller remembers to call `WslcCloseSession`.

### 3. Process-Scoped, Publish-Once Semantics

**Decision**: The CLI session is published once during CLI initialization and is never replaced or cleared during the process lifetime.

**Rationale**: This provides a strong invariant that simplifies reasoning about the API:

- **Publish-once**: After `WslcGetCLISession` returns `S_OK` for a given process, it will always return the same session handle (with a new reference) for the remainder of the process lifetime.
- **Never cleared**: The CLI runtime does not unpublish the session. Even during CLI shutdown, the session remains accessible (but see Lifetime note above — behavior is undefined during CRT static destruction). Ref-counting ensures the session is only destroyed when all references (including the CLI's own reference) are released.
- **No replacement**: Calling `WslcGetCLISession` at different times within the same process always returns the same underlying session.

### 4. Internal Publication Mechanism

The CLI runtime publishes its session via an **internal** (non-exported) function:

```c
// Internal — not part of the public API surface
void WslcPublishCLISession(_In_ WslcSession session);
```

This function:

1. Stores the session in a process-global atomic pointer with release semantics.
2. Calls `AddRef` on the session (the CLI retains its own reference separately).
3. Silently ignores subsequent calls (the `std::call_once` guard ensures only the first invocation takes effect). Debug builds should assert that this function is not called more than once.

The publication happens during the WSLC toolchain initialization — when `wslc` CLI starts, when MSBuild targets load the WSLC library, or when the IDE integration initializes the container environment — after the session is fully initialized but before any build/run/debug operations begin.

### 6. Implementation Location

The new API is implemented within the WSLC library. The following files are involved:

| File | Action | Purpose |
|------|--------|---------|
| `src/windows/wslc/inc/wslc.h` | **Modify** | Public header — add `WslcGetCLISession` declaration and `WSLC_E_NO_CLI_SESSION` error code |
| `src/windows/wslc/core/cli_session.h` | **Add new** | Internal header — declare `WslcPublishCLISession` |
| `src/windows/wslc/core/cli_session.cpp` | **Add new** | Implementation — `g_cliSession` atomic, `WslcPublishCLISession`, `WslcGetCLISession` |
| `src/windows/wslc/wslc.def` | **Modify** | DLL exports — add `WslcGetCLISession` entry |
| `src/windows/wslc/core/session.cpp` | **Modify** | Existing session init — call `WslcPublishCLISession` after session creation |
| `src/windows/wslc/core/session.h` | **Reference only** | Existing session internals — understand `WslcSession_s` and `WslcSessionAddRef` |
| `src/windows/wslc/CMakeLists.txt` | **Modify** | Build config — add `core/cli_session.cpp` to source list |
| `test/windows/wslc/GetCLISessionTests.cpp` | **Add new** | Unit tests for the new API |

> **Note**: The exact file paths follow the WSLC project structure convention. The key principle is: the public declaration goes in the public header (`wslc.h`), the implementation goes in the `core/` subdirectory alongside other session management code, and the export goes in the `.def` file.

### 5. Naming: `WslcGetCLISession`

**Decision**: Use `WslcGetCLISession`.

| Alternative | Why Rejected |
|-------------|-------------|
| `WslcGetCurrentSession` | Ambiguous — "current" could mean the most recently created session |
| `WslcGetProcessSession` | Too generic — doesn't convey it's the toolchain-established session |
| `WslcAcquireCLISession` | "Acquire" implies lock semantics or exclusive access |
| `WslcGetDefaultSession` | Confusing — "default" has a different meaning in WSL (default distro) |

`WslcGetCLISession` is clear: it returns the session established by the WSLC CLI toolchain. The `Get` prefix aligns with existing WSL patterns (`GetWslConfigFilePath`, `GetDefaultDistribution`, `GetDistributionId`). The name remains appropriate even when called from non-CLI contexts (MSBuild tasks, IDE hosts) because the session originates from the CLI/toolchain layer.

## Implementation Sketch

### Session Storage

```cpp
// wslc_cli_session.cpp (internal)
namespace {
    std::atomic<WslcSession_s*> g_cliSession{nullptr};
}
```

### Publication (CLI startup)

```cpp
void WslcPublishCLISession(WslcSession session)
{
    // CAS ensures only the first caller succeeds; subsequent calls
    // see non-null and hit the assert.
    WslcSession_s* expected = nullptr;
    if (g_cliSession.compare_exchange_strong(expected, session, std::memory_order_release, std::memory_order_relaxed))
    {
        // AddRef for the global reference (intentionally leaked — see below)
        WslcSessionAddRef(session);
    }
    else
    {
        assert(false && "CLI session published more than once");
    }
}
```

> **Global reference lifetime**: The `AddRef` performed by `WslcPublishCLISession` is intentionally never released. The global reference is "leaked" and reclaimed by process exit. This is a deliberate design choice — there is no safe point during shutdown to release the global reference because other code may still hold derived references obtained from `WslcGetCLISession`. When the Windows app closes or is terminated, the OS reclaims all process memory including ref counts — no cleanup is needed. The session's destructor-side cleanup (e.g., closing hvsocket connections) is handled by the toolchain's own reference, which it releases during its normal shutdown path.

### Retrieval (Public API)

```cpp
STDAPI WslcGetCLISession(_Out_ WslcSession* session)
{
    RETURN_HR_IF(E_POINTER, session == nullptr);
    *session = nullptr;

    // Acquire load pairs with release store in WslcPublishCLISession.
    // Readers that run before publication see nullptr (-> WSLC_E_NO_CLI_SESSION).
    auto* raw = g_cliSession.load(std::memory_order_acquire);
    RETURN_HR_IF(WSLC_E_NO_CLI_SESSION, raw == nullptr);

    // AddRef for the caller's reference
    WslcSessionAddRef(raw);
    *session = raw;
    return S_OK;
}
```

### Thread Safety Analysis

| Operation | Synchronization | Notes |
|-----------|----------------|-------|
| `g_cliSession` write | `compare_exchange_strong` + release | Exactly once; CAS guarantees atomicity; second publish asserts in debug |
| `g_cliSession` read | Acquire load | Sees fully initialized session or nullptr (→ `WSLC_E_NO_CLI_SESSION`) |
| `WslcSessionAddRef` | Internal atomic increment | Standard ref-count thread safety |
| Session operations | Internal session locks | All `WslcSession` operations are synchronized |
| Global reference cleanup | Intentional leak | Reclaimed by process exit; see "Global reference lifetime" note |

## Usage Example

### Inner-Loop: Build, Run, and Debug

The `#ifdef _DEBUG` guard controls **only** session acquisition — in debug builds, the app reuses the CLI session established by the WSLC toolchain; in release builds, it creates its own. The rest of the container lifecycle code is identical:

```c
// --- Session acquisition (the only part that differs) ---
WslcSession session = NULL;

#ifdef _DEBUG
// Debug build: reuse the session published by the WSLC toolchain
// (wslc CLI, MSBuild targets, or IDE integration)
HRESULT hr = WslcGetCLISession(&session);
#else
// Release build: create a standalone session
WslcSessionConfig config = {};
HRESULT hr = WslcCreateSession(&config, &session);
#endif

if (FAILED(hr))
{
    return hr;
}

// --- Everything below is the same for debug and release ---

// Create and run the container
WslcContainerConfig containerConfig = {};
WslcInitContainerConfig("my-image:latest", &containerConfig);

WslcContainer container = NULL;
hr = WslcCreateContainer(session, &containerConfig, &container);
if (FAILED(hr))
{
    WslcCloseSession(session);
    return hr;
}

hr = WslcStartContainer(container, WSLC_CONTAINER_START_FLAG_NONE);
// ... container is running ...

WslcCloseContainer(container);

// Release our reference (does NOT close the CLI session in debug builds)
WslcCloseSession(session);
```

## Testing Strategy

### Unit Tests

| Test Case | Description |
|-----------|-------------|
| `GetCLISession_BeforePublish` | Call `WslcGetCLISession` before any session is published. Expect `WSLC_E_NO_CLI_SESSION`. |
| `GetCLISession_AfterPublish` | Publish a session, then call `WslcGetCLISession`. Expect `S_OK` and valid handle. |
| `GetCLISession_NullParam` | Pass `NULL` output parameter. Expect `E_POINTER`. |
| `GetCLISession_RefCounting` | Call `WslcGetCLISession` twice, verify both handles are valid and independent. Close one, verify the other still works. |
| `GetCLISession_ConcurrentAccess` | Call `WslcGetCLISession` from multiple threads simultaneously. Verify all succeed and return the same underlying session. |
| `GetCLISession_SessionIdentity` | Verify that multiple calls return handles to the same underlying session (same session ID / properties). |
| `GetCLISession_PublishOnce` | Attempt to publish a second session. Verify the invariant holds (assertion fires / second publish is ignored). |

### Integration Tests

| Test Case | Description |
|-----------|-------------|
| `CLISession_BuildAndRun` | Run `wslc build && wslc run`, verify that application code calling `WslcGetCLISession` during the run receives the same session the toolchain created. |
| `CLISession_ProcessExit` | Obtain a session handle, then exit the process without calling `WslcCloseSession`. Verify no resource leak or crash (OS reclaims ref counts). |

## Appendix: Relationship to Existing WSL APIs

| Existing Pattern | WSLC Equivalent | Notes |
|-----------------|----------------|-------|
| `WslConfig_t` opaque handle | `WslcSession` opaque handle | Same pattern: typedef to opaque struct pointer |
| `CreateWslConfig` / `FreeWslConfig` | `WslcCreateSession` / `WslcCloseSession` | Create/Free lifecycle pair |
| `GetWslConfigFilePath` (returns existing) | `WslcGetCLISession` (returns existing) | "Get" = retrieve, not create |
| `HRESULT` return codes | `HRESULT` return codes | Standard Win32 error handling |
| `WSL_E_*` custom errors | `WSLC_E_*` custom errors | FACILITY_ITF with distinct ranges |
| `ILxssUserSession` per-user COM singleton | CLI session per-process singleton | Different scope, same singleton concept |
