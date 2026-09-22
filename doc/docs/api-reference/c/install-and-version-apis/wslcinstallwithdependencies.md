# WslcInstallWithDependencies

```c
STDAPI WslcInstallWithDependencies(
    _In_ WslcComponentFlags components,
    _In_ WslcInstallOptions options,
    _In_opt_ WslcInstallCallback progressCallback,
    _In_opt_ PVOID context);
```

| Parameter | Type | Direction |
|---|---|---|
| `components` | `WslcComponentFlags` | in |
| `options` | `WslcInstallOptions` | in |
| `progressCallback` | `WslcInstallCallback` | in, optional |
| `context` | `PVOID` | in, optional |

Return value: `HRESULT`.

Header note: callbacks are only made for components actively installed by this call. That list can be acquired beforehand with [`WslcGetMissingComponents`](wslcgetmissingcomponents.md).

`WSLC_COMPONENT_FLAG_SDK_NEEDS_UPDATE` reports that the client SDK must be updated. If `components`
contains this flag, the function immediately returns `WSLC_E_SDK_UPDATE_NEEDED` without installing
any components because the running SDK cannot update itself. Handle the flag separately and remove
it before installing any remaining components.

Example:

```c
void CALLBACK OnInstallProgress(
    WslcComponentFlags component,
    uint32_t progressSteps,
    uint32_t totalSteps,
    PVOID context)
{
    UNREFERENCED_PARAMETER(context);
    printf("component=%u %u/%u\n", (unsigned)component, progressSteps, totalSteps);
}

WslcComponentFlags missing = WSLC_COMPONENT_FLAG_NONE;
HRESULT hr = WslcGetMissingComponents(&missing);
if (SUCCEEDED(hr))
{
    if ((missing & WSLC_COMPONENT_FLAG_SDK_NEEDS_UPDATE) != 0)
    {
        printf("Update the Microsoft.WSL.Containers SDK package.\n");
        missing = (WslcComponentFlags)(missing & ~WSLC_COMPONENT_FLAG_SDK_NEEDS_UPDATE);
    }

    if (missing != WSLC_COMPONENT_FLAG_NONE)
    {
        hr = WslcInstallWithDependencies(
            missing,
            WSLC_INSTALL_OPTION_NONE,
            OnInstallProgress,
            NULL);
    }
}
```
