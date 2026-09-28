# WslcSetSessionSettingsCpuCount

```c
STDAPI WslcSetSessionSettingsCpuCount(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t cpuCount);
```

| Parameter | Type | Direction |
|---|---|---|
| `sessionSettings` | `WslcSessionSettings*` | in |
| `cpuCount` | `uint32_t` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcSetSessionSettingsCpuCount(&sessionSettings, (uint32_t)4);
```
