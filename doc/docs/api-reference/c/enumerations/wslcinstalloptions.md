# WslcInstallOptions

Controls [`WslcInstallWithDependencies`](../install-and-version-apis/wslcinstallwithdependencies.md).

```c
typedef enum WslcInstallOptions
{
    WSLC_INSTALL_OPTION_NONE = 0,
    WSLC_INSTALL_OPTION_REPAIR = 1,
} WslcInstallOptions;
```

| Enumerator | Meaning |
|---|---|
| `WSLC_INSTALL_OPTION_NONE` | Use the normal installation or update behavior for the selected components. |
| `WSLC_INSTALL_OPTION_REPAIR` | Request repair behavior for the selected components, including resetting WSL package registration before updating it. |
