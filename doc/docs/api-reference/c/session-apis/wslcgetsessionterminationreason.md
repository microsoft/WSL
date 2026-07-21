# WslcGetSessionTerminationReason

```c
STDAPI WslcGetSessionTerminationReason(_In_ WslcSession session, _Out_ WslcSessionTerminationReason* reason);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `reason` | `WslcSessionTerminationReason*` | out |

Return value: `HRESULT`.

Example:

```c
WslcSessionTerminationReason reason = WSLC_SESSION_TERMINATION_REASON_UNKNOWN;
HRESULT hr = WslcGetSessionTerminationReason(session, &reason);
```
