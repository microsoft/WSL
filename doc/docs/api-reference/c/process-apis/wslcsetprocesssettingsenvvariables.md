# WslcSetProcessSettingsEnvVariables

```c
STDAPI WslcSetProcessSettingsEnvVariables(_In_ WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* key_value, size_t argc);
```

| Parameter | Type | Direction |
|---|---|---|
| `processSettings` | `WslcProcessSettings*` | in |
| `key_value` | `PCSTR const*` | in |
| `argc` | `size_t` | in |

Return value: `HRESULT`.

Example:

```c
PCSTR const key_value[] = { "HOME=/root", "DEMO_FLAG=1" };
HRESULT hr = WslcSetProcessSettingsEnvVariables(&processSettings, key_value, _countof(key_value));
```
