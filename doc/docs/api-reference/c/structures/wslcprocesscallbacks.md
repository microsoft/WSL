# WslcProcessCallbacks

```c
typedef struct WslcProcessCallbacks
{
    WslcStdIOCallback onStdOut;
    WslcStdIOCallback onStdErr;
    WslcProcessExitCallback onExit;
} WslcProcessCallbacks;
```

| Field | Type |
|---|---|
| `onStdOut` | `WslcStdIOCallback` |
| `onStdErr` | `WslcStdIOCallback` |
| `onExit` | `WslcProcessExitCallback` |
