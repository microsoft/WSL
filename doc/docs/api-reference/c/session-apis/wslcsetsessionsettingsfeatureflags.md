# WslcSetSessionSettingsFeatureFlags

```c
STDAPI WslcSetSessionSettingsFeatureFlags(_In_ WslcSessionSettings* sessionSettings, _In_ WslcSessionFeatureFlags flags);
```

| Parameter | Type | Direction |
|---|---|---|
| `sessionSettings` | `WslcSessionSettings*` | in |
| `flags` | `WslcSessionFeatureFlags` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcSetSessionSettingsFeatureFlags(
    &sessionSettings,
    WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU);
```
