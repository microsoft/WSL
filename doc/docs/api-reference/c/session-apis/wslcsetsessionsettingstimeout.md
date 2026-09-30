# WslcSetSessionSettingsTimeout

```c
STDAPI WslcSetSessionSettingsTimeout(_In_ WslcSessionSettings* sessionSettings, _In_ uint32_t timeoutMS);
```

| Parameter | Type | Direction |
|---|---|---|
| `sessionSettings` | `WslcSessionSettings*` | in |
| `timeoutMS` | `uint32_t` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcSetSessionSettingsTimeout(&sessionSettings, (uint32_t)120000);
```
