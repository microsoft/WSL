# WslcSetSessionSettingsFeatureFlags

```c
STDAPI WslcSetSessionSettingsFeatureFlags(_In_ WslcSessionSettings* sessionSettings, _In_ WslcSessionFeatureFlags flags);
```

| Parameter | Type | Direction |
|---|---|---|
| `sessionSettings` | `WslcSessionSettings*` | in |
| `flags` | `WslcSessionFeatureFlags` | in |

Return value: `HRESULT`.

`WSLC_SESSION_FEATURE_FLAG_LARGE_MEMORY_BACKING_PAGES` requires WSL 3.1.0 or later.
The runtime version is checked by `WslcCreateSession`, not by this settings setter.

Example:

```c
HRESULT hr = WslcSetSessionSettingsFeatureFlags(
    &sessionSettings,
    WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU);
```
