# WslcSignalProcess

```c
STDAPI WslcSignalProcess(_In_ WslcProcess process, _In_ WslcSignal signal);
```

| Parameter | Type | Direction |
|---|---|---|
| `process` | `WslcProcess` | in |
| `signal` | `WslcSignal` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcSignalProcess(process, WSLC_SIGNAL_SIGTERM);
```
