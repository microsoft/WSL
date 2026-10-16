# WslcProcessExitCallback

```c
typedef __callback void(CALLBACK* WslcProcessExitCallback)(INT32 exitCode, _In_opt_ PVOID context);
```

| Parameter | Type |
|---|---|
| `exitCode` | `INT32` |
| `context` | `PVOID` |

The callback runs after buffered process I/O has been delivered and registered I/O callbacks have
returned.
