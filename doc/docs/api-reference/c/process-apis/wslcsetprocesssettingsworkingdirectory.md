# WslcSetProcessSettingsWorkingDirectory

```c
STDAPI WslcSetProcessSettingsWorkingDirectory(_In_ WslcProcessSettings* processSettings, _In_ PCSTR workingDirectory);
```

| Parameter | Type | Direction |
|---|---|---|
| `processSettings` | `WslcProcessSettings*` | in |
| `workingDirectory` | `PCSTR` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcSetProcessSettingsWorkingDirectory(&processSettings, "/work");
```
