# WslcContainerStartFlags

```c
typedef enum WslcContainerStartFlags
{
    WSLC_CONTAINER_START_FLAG_NONE = 0x00000000,
    WSLC_CONTAINER_START_FLAG_ATTACH = 0x00000001,
    WSLC_CONTAINER_START_FLAG_DEFER_IO_CALLBACKS = 0x00000002,

} WslcContainerStartFlags;
```

| Enumerator | Value | Description |
|---|---|---|
| `WSLC_CONTAINER_START_FLAG_NONE` | `0x00000000` | No optional behavior. |
| `WSLC_CONTAINER_START_FLAG_ATTACH` | `0x00000001` | Attaches the init process standard I/O. Required when init process I/O callbacks are registered. |
| `WSLC_CONTAINER_START_FLAG_DEFER_IO_CALLBACKS` | `0x00000002` | Registers the init process I/O callbacks without delivering them until [WslcActivateContainerIOCallbacks](../container-apis/wslcactivatecontaineriocallbacks.md) is called. Has no effect if no callbacks are registered. |

Used by [WslcStartContainer](../container-apis/wslcstartcontainer.md).
