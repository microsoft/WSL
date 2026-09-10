# WslcStartContainer

```c
STDAPI WslcStartContainer(_In_ WslcContainer container, _In_ WslcContainerStartFlags flags, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `container` | `WslcContainer` | in |
| `flags` | `WslcContainerStartFlags` | in |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcStartContainer(container, WSLC_CONTAINER_START_FLAG_ATTACH, NULL);
```
