# WslcProcessFlags

```c
typedef enum WslcProcessFlags
{
    WSLC_PROCESS_FLAG_NONE = 0x00000000,
    WSLC_PROCESS_FLAG_STDIN = 0x00000001,
} WslcProcessFlags;
```

| Enumerator | Value | Description |
|---|---|---|
| `WSLC_PROCESS_FLAG_NONE` | `0x00000000` | No optional behavior. |
| `WSLC_PROCESS_FLAG_STDIN` | `0x00000001` | Enables standard input for the process. Without it the process observes an immediately closed stdin and `WSLC_PROCESS_IO_HANDLE_STDIN` cannot be used. |

Set with [WslcSetProcessSettingsFlags](../process-apis/wslcsetprocesssettingsflags.md).
