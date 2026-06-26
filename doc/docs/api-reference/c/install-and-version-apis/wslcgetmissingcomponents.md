# WslcGetMissingComponents

```c
STDAPI WslcGetMissingComponents(_Out_ WslcComponentFlags* missingComponents);
```

| Parameter | Type | Direction |
|---|---|---|
| `missingComponents` | `WslcComponentFlags*` | out |

Return value: `HRESULT`.

Example:

```c
WslcComponentFlags missingComponents = WSLC_COMPONENT_FLAG_NONE;
HRESULT hr = WslcGetMissingComponents(&missingComponents);
```
