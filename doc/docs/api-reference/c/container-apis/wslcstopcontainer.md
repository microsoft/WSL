# WslcStopContainer

```c
STDAPI WslcStopContainer(_In_ WslcContainer container, _In_ WslcSignal signal, _In_ uint32_t timeoutSeconds, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `container` | `WslcContainer` | in |
| `signal` | `WslcSignal` | in |
| `timeoutSeconds` | `uint32_t` | in |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcStopContainer(
    container,
    WSLC_SIGNAL_SIGTERM,
    (uint32_t)30,
    NULL);
```
