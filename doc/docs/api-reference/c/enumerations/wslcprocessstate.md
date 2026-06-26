# WslcProcessState

```c
typedef enum WslcProcessState
{
    WSLC_PROCESS_STATE_UNKNOWN = 0,
    WSLC_PROCESS_STATE_RUNNING = 1,
    WSLC_PROCESS_STATE_EXITED = 2,
    WSLC_PROCESS_STATE_SIGNALLED = 3
} WslcProcessState;
```

| Enumerator | Value |
|---|---|
| `WSLC_PROCESS_STATE_UNKNOWN` | `0` |
| `WSLC_PROCESS_STATE_RUNNING` | `1` |
| `WSLC_PROCESS_STATE_EXITED` | `2` |
| `WSLC_PROCESS_STATE_SIGNALLED` | `3` |
