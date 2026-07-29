# WslcSetSessionSettingsMemory

```c
STDAPI WslcSetSessionSettingsMemory(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t memoryMB);
```

| Parameter | Type | Direction |
|---|---|---|
| `sessionSettings` | `WslcSessionSettings*` | in |
| `memoryMB` | `uint32_t` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcSetSessionSettingsMemory(&sessionSettings, (uint32_t)4096);
```
