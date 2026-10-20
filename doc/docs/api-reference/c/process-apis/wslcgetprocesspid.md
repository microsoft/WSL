# WslcGetProcessPid

```c
STDAPI WslcGetProcessPid(_In_ WslcProcess process, _Out_ uint32_t* pid);
```

| Parameter | Type | Direction |
|---|---|---|
| `process` | `WslcProcess` | in |
| `pid` | `uint32_t*` | out |

Return value: `HRESULT`.

Example:

```c
uint32_t pid = 0;
HRESULT hr = WslcGetProcessPid(process, &pid);
```
