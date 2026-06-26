# WslcGetProcessState

```c
STDAPI WslcGetProcessState(_In_ WslcProcess process, _Out_ WslcProcessState* state);
```

| Parameter | Type | Direction |
|---|---|---|
| `process` | `WslcProcess` | in |
| `state` | `WslcProcessState*` | out |

Return value: `HRESULT`.

Example:

```c
WslcProcessState state = WSLC_PROCESS_STATE_UNKNOWN;
HRESULT hr = WslcGetProcessState(process, &state);
```
