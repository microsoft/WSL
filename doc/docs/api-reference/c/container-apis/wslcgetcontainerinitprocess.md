# WslcGetContainerInitProcess

```c
STDAPI WslcGetContainerInitProcess(_In_ WslcContainer container, _Out_ WslcProcess* initProcess);
```

| Parameter | Type | Direction |
|---|---|---|
| `container` | `WslcContainer` | in |
| `initProcess` | `WslcProcess*` | out |

Return value: `HRESULT`.

Example:

```c
WslcProcess initProcess = NULL;
HRESULT hr = WslcGetContainerInitProcess(container, &initProcess);
```
