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
| `WSLC_INSTALL_OPTION_REPAIR` | Allow the WSL package to be reinstalled. This option has no effect when installing Virtual Machine Platform. |
