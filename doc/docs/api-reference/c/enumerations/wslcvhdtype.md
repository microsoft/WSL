# WslcVhdType

```c
typedef enum WslcVhdType
{
    WSLC_VHD_TYPE_DYNAMIC = 0, // Expanding VHDX (default)
    WSLC_VHD_TYPE_FIXED = 1,   // Fixed-allocation VHDX (only honored by WslcCreateSessionVhdVolume)
    WSLC_VHD_TYPE_SPARSE = 2   // Expanding sparse VHDX (experimental)
} WslcVhdType;
```

| Enumerator | Value |
|---|---|
| `WSLC_VHD_TYPE_DYNAMIC` | `0` |
| `WSLC_VHD_TYPE_FIXED` | `1` |
| `WSLC_VHD_TYPE_SPARSE` | `2` |

`WSLC_VHD_TYPE_SPARSE` creates a dynamically expanding VHD backed by a sparse host file. This feature is currently experimental.
