# WslcGetProcessExitEvent

```c
STDAPI WslcGetProcessExitEvent(_In_ WslcProcess process, _Out_ HANDLE* exitEvent);
```

| Parameter | Type | Direction |
|---|---|---|
| `process` | `WslcProcess` | in |
| `exitEvent` | `HANDLE*` | out |

Return value: `HRESULT`.

Example:

```c
HANDLE exitEvent = NULL;
HRESULT hr = WslcGetProcessExitEvent(process, &exitEvent);
if (SUCCEEDED(hr))
{
    WaitForSingleObject(exitEvent, INFINITE);
}
```
