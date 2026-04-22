# Slow Operation Telemetry

WSL emits `SlowOperationStarted` / `SlowOperationEnded` events when a guarded step in the
create-instance flow (HCS operations, guest boot waits, networking setup, plugin hooks,
init-daemon waits) exceeds a fixed slow threshold. This replaces the earlier
Begin/End-per-step design: on a fast startup, **no** new events are emitted. Events only
show up when a step is actually slow, which makes triage queries straightforward.

## Thresholds

| Threshold | Default | What happens |
|---|---|---|
| Slow | 10 s | `SlowOperationStarted` telemetry + `WSL_LOG` debug entry |
| User-visible | 60 s | Plus a warning surfaced to the user via `EMIT_USER_WARNING` |

Both thresholds are encoded in `WslSlowOperation` (see `src/windows/common/WslSlowOperation.h`).
The RAII guard emits a single timestamped `SlowOperationStarted` when its threadpool timer
fires, re-arms for the remainder of the user-visible threshold, and emits
`SlowOperationEnded` with the final `elapsedMs` and `hr` on destruction — including when
the scope exits via exception.

## Events

### `SlowOperationStarted`

Emitted once per guarded scope, only when the slow threshold is crossed.

| Field | Type | Notes |
|---|---|---|
| `wslVersion` | string | Via `WSL_LOG_TELEMETRY` |
| `name` | string | Phase identifier (see table below) |
| `thresholdMs` | int64 | The threshold that was crossed (10000) |

### `SlowOperationEnded`

Emitted on scope exit, only when `SlowOperationStarted` was already emitted for that scope.

| Field | Type | Notes |
|---|---|---|
| `wslVersion` | string | Via `WSL_LOG_TELEMETRY` |
| `name` | string | Matches the Started event |
| `elapsedMs` | int64 | Total wall-clock time the scope took |
| `userVisible` | bool | True if the user-visible threshold was also crossed |
| `hr` | HRESULT | `S_OK` on normal exit, `E_FAIL` if the scope exited via exception |

A `SlowOperationStarted` without a matching `SlowOperationEnded` indicates a true silent
hang (process terminated before the scope returned). Pair by thread-id + `name` + time.

## Guarded phases

| `name` | Typical cause when slow |
|---|---|
| `HcsCreateSystem`, `HcsStartSystem` | HCS service unresponsive |
| `WaitForMiniInitConnect`, `ReadGuestCapabilities` | Kernel boot / hvsocket broken |
| `ConfigureNetworking`, `CreateNatNetwork` | HNS slow (common false-positive source) |
| `InitializeGuest` | Guest config message stuck |
| `AttachDistroVhd` | VHD attach on bad / BitLocker storage |
| `SendLaunchInit`, `WaitForInitDaemonConnect` | hvsocket write / guest init start |
| `WaitForCreateInstanceResult` | ext4 mount / journal recovery |
| `WaitForDrvFsInit` | Plan9 / virtiofs setup |
| `WaitForInitConfigResponse` | systemd startup |
| `PluginOnVmStarted`, `PluginOnDistributionStarted` | 3rd-party plugin (e.g. Docker Desktop) |

## Backend integration

No state-machine changes are required on the backend. Queries that previously looked at
the opaque `timeout` bucket for `CreateInstance` can now be refined by joining
`SlowOperationStarted` on the same process/thread within the timeout window:

- `SlowOperationStarted` present, `SlowOperationEnded` absent → true hang, bucket by `name`.
- Both present with `hr == S_OK` → slow but completed. `elapsedMs` gives the actual duration.
- Both present with `hr != S_OK` → slow and surfaced a failure; the normal error events
  carry the root cause.
