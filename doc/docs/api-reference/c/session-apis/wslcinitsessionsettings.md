# WslcInitSessionSettings

```c
STDAPI WslcInitSessionSettings(_In_ PCWSTR name, _In_ PCWSTR storagePath, _Out_ WslcSessionSettings* sessionSettings);
```

| Parameter | Type | Direction |
|---|---|---|
| `name` | `PCWSTR` | in |
| `storagePath` | `PCWSTR` | in |
| `sessionSettings` | `WslcSessionSettings*` | out |

Return value: `HRESULT`.

Example:

```c
WslcSessionSettings sessionSettings;
HRESULT hr = WslcInitSessionSettings(
    L"demo-session",
    L"C:\\WSLC\\demo-session",
    &sessionSettings);
```
