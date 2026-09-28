# WslcGetProcessExitCode

```c
STDAPI WslcGetProcessExitCode(_In_ WslcProcess process, _Out_ PINT32 exitCode);
```

| Parameter | Type | Direction |
|---|---|---|
| `process` | `WslcProcess` | in |
| `exitCode` | `PINT32` | out |

Return value: `HRESULT`.

Example:

```c
INT32 exitCode = 0;
HRESULT hr = WslcGetProcessExitCode(process, &exitCode);
```
