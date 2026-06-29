# WslcSignal

```c
typedef enum WslcSignal
{
    WSLC_SIGNAL_NONE = 0,     // No signal; reserved for future use
    WSLC_SIGNAL_SIGHUP = 1,   // SIGHUP: reload / hangup
    WSLC_SIGNAL_SIGINT = 2,   // SIGINT: interrupt (Ctrl-C)
    WSLC_SIGNAL_SIGQUIT = 3,  // SIGQUIT: quit with core dump
    WSLC_SIGNAL_SIGKILL = 9,  // SIGKILL: immediate termination
    WSLC_SIGNAL_SIGTERM = 15, // SIGTERM: graceful shutdown
} WslcSignal;
```

| Enumerator | Value |
|---|---|
| `WSLC_SIGNAL_NONE` | `0` |
| `WSLC_SIGNAL_SIGHUP` | `1` |
| `WSLC_SIGNAL_SIGINT` | `2` |
| `WSLC_SIGNAL_SIGQUIT` | `3` |
| `WSLC_SIGNAL_SIGKILL` | `9` |
| `WSLC_SIGNAL_SIGTERM` | `15` |
