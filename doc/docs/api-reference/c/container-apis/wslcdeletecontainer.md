# WslcDeleteContainer

```c
STDAPI WslcDeleteContainer(_In_ WslcContainer container, _In_ WslcDeleteContainerFlags flags, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `container` | `WslcContainer` | in |
| `flags` | `WslcDeleteContainerFlags` | in |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcDeleteContainer(
    container,
    WSLC_DELETE_CONTAINER_FLAG_FORCE,
    NULL);
```
