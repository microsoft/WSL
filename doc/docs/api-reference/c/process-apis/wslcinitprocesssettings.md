# WslcInitProcessSettings

```c
STDAPI WslcInitProcessSettings(_Out_ WslcProcessSettings* processSettings);
```

| Parameter | Type | Direction |
|---|---|---|
| `processSettings` | `WslcProcessSettings*` | out |

Return value: `HRESULT`.

Example:

```c
WslcProcessSettings processSettings;
HRESULT hr = WslcInitProcessSettings(&processSettings);
```
