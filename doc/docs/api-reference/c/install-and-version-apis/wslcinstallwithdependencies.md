# WslcInstallWithDependencies

```c
STDAPI WslcInstallWithDependencies(_In_opt_ WslcInstallCallback progressCallback, _In_opt_ PVOID context);
```

| Parameter | Type | Direction |
|---|---|---|
| `progressCallback` | `WslcInstallCallback` | in, optional |
| `context` | `PVOID` | in, optional |

Return value: `HRESULT`.

Header note: callbacks are only made for components actively installed by this call. That list can be acquired beforehand with [`WslcGetMissingComponents`](wslcgetmissingcomponents.md).

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

HRESULT hr = WslcInstallWithDependencies(OnInstallProgress, NULL);
```

---
