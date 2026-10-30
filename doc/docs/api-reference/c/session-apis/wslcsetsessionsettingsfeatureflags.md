# WslcSetSessionSettingsFeatureFlags

```c
STDAPI WslcSetSessionSettingsFeatureFlags(_In_ WslcSessionSettings* sessionSettings, _In_ WslcSessionFeatureFlags flags);
```

| Parameter | Type | Direction |
|---|---|---|
| `sessionSettings` | `WslcSessionSettings*` | in |
| `flags` | `WslcSessionFeatureFlags` | in |

Return value: `HRESULT`.

The nested virtualization flag requires WSL 3.1.0 or later. Runtime support is checked by
`WslcCreateSession`, not when storing these settings.

Example:

```c
HRESULT hr = WslcSetSessionSettingsFeatureFlags(
    &sessionSettings,
    WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU |
        WSLC_SESSION_FEATURE_FLAG_NESTED_VIRTUALIZATION);
```
