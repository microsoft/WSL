# WslcGetProcessIOHandle

```c
STDAPI WslcGetProcessIOHandle(_In_ WslcProcess process, _In_ WslcProcessIOHandle ioHandle, _Out_ HANDLE* handle);
```

| Parameter | Type | Direction |
|---|---|---|
| `process` | `WslcProcess` | in |
| `ioHandle` | `WslcProcessIOHandle` | in |
| `handle` | `HANDLE*` | out |

Return value: `HRESULT`.

Example:

```c
HANDLE stdoutHandle = NULL;
HRESULT hr = WslcGetProcessIOHandle(process, WSLC_PROCESS_IO_HANDLE_STDOUT, &stdoutHandle);
```
