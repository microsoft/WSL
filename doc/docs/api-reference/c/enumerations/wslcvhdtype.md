# WslcVhdType

```c
typedef enum WslcVhdType
{
    WSLC_VHD_TYPE_DYNAMIC = 0, // Expanding VHDX (default)
    WSLC_VHD_TYPE_FIXED = 1    // Fixed-allocation VHDX (only honored by WslcCreateSessionVhdVolume)
} WslcVhdType;
```

| Enumerator | Value |
|---|---|
| `WSLC_VHD_TYPE_DYNAMIC` | `0` |
| `WSLC_VHD_TYPE_FIXED` | `1` |
