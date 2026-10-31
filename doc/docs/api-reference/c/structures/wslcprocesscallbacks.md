# WslcProcessCallbacks

```c
typedef struct WslcProcessCallbacks
{
    WslcStdIOCallback onStdOut;
    WslcStdIOCallback onStdErr;
    WslcProcessExitCallback onExit;
} WslcProcessCallbacks;
```

| Field | Type | Description |
|---|---|---|
| `onStdOut` | [`WslcStdIOCallback`](../callback-types/wslcstdiocallback.md) | Receives data written to standard output. |
| `onStdErr` | [`WslcStdIOCallback`](../callback-types/wslcstdiocallback.md) | Receives data written to standard error. |
| `onExit` | [`WslcProcessExitCallback`](../callback-types/wslcprocessexitcallback.md) | Receives the process exit code after buffered I/O has been delivered and the I/O callbacks have returned. |
