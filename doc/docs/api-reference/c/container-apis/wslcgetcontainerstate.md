# WslcGetContainerState

```c
STDAPI WslcGetContainerState(_In_ WslcContainer container, _Out_ WslcContainerState* state);
```

| Parameter | Type | Direction |
|---|---|---|
| `container` | `WslcContainer` | in |
| `state` | `WslcContainerState*` | out |

Return value: `HRESULT`.

Example:

```c
WslcContainerState state = WSLC_CONTAINER_STATE_INVALID;
HRESULT hr = WslcGetContainerState(container, &state);
```
