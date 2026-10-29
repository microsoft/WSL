# WslcSessionFeatureFlags

```c
typedef enum WslcSessionFeatureFlags
{
    WSLC_SESSION_FEATURE_FLAG_NONE = 0x00000000,
    WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU = 0x00000004,
    WSLC_SESSION_FEATURE_FLAG_LARGE_MEMORY_BACKING_PAGES = 0x00000040
} WslcSessionFeatureFlags;
```

| Enumerator | Value |
|---|---|
| `WSLC_SESSION_FEATURE_FLAG_NONE` | `0x00000000` |
| `WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU` | `0x00000004` |
| `WSLC_SESSION_FEATURE_FLAG_LARGE_MEMORY_BACKING_PAGES` | `0x00000040` |

`WSLC_SESSION_FEATURE_FLAG_LARGE_MEMORY_BACKING_PAGES` opts the session VM in to 2 MiB host backing pages.
This may improve performance for memory-intensive workloads at the cost of increased host memory usage and allocation latency.
It is disabled by default and is independent of `.wslconfig`.
It requires WSL 3.1.0 or later. On older runtimes, `WslcCreateSession` returns `WSLC_E_RUNTIME_UPDATE_NEEDED`
with an error message containing the installed and required WSL versions.
