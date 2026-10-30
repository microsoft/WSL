# WslcSessionFeatureFlags

```c
typedef enum WslcSessionFeatureFlags
{
    WSLC_SESSION_FEATURE_FLAG_NONE = 0x00000000,
    WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU = 0x00000004,
    WSLC_SESSION_FEATURE_FLAG_NESTED_VIRTUALIZATION = 0x00000040
} WslcSessionFeatureFlags;
```

| Enumerator | Value |
|---|---|
| `WSLC_SESSION_FEATURE_FLAG_NONE` | `0x00000000` |
| `WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU` | `0x00000004` |
| `WSLC_SESSION_FEATURE_FLAG_NESTED_VIRTUALIZATION` | `0x00000040` |

Nested virtualization requires WSL 3.1.0 or later and host support for virtualization extensions.
On an older runtime, `WslcCreateSession` returns `WSLC_E_RUNTIME_UPDATE_NEEDED`.
