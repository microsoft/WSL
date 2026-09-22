# WslcComponentFlags

```c
typedef enum WslcComponentFlags
{
    WSLC_COMPONENT_FLAG_NONE = 0,
    WSLC_COMPONENT_FLAG_VIRTUAL_MACHINE_PLATFORM = 1,
    WSLC_COMPONENT_FLAG_WSL_PACKAGE = 2,
    WSLC_COMPONENT_FLAG_SDK_NEEDS_UPDATE = 4,
} WslcComponentFlags;
```

| Enumerator | Value |
|---|---|
| `WSLC_COMPONENT_FLAG_NONE` | `0` |
| `WSLC_COMPONENT_FLAG_VIRTUAL_MACHINE_PLATFORM` | `1` |
| `WSLC_COMPONENT_FLAG_WSL_PACKAGE` | `2` |
| `WSLC_COMPONENT_FLAG_SDK_NEEDS_UPDATE` | `4` |

`WSLC_COMPONENT_FLAG_SDK_NEEDS_UPDATE` indicates that the SDK bundled with the calling application
is incompatible with the installed runtime. It is not an installable component. The application
should report that an application update is required; its developer must update the
`Microsoft.WSL.Containers` dependency and publish a new version. Do not pass this flag to
`WslcInstallWithDependencies`.

---
