# WslcSetContainerSettingsHostName

```c
STDAPI WslcSetContainerSettingsHostName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR hostName);
```

| Parameter | Type | Direction |
|---|---|---|
| `containerSettings` | `WslcContainerSettings*` | in |
| `hostName` | `PCSTR` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcSetContainerSettingsHostName(&containerSettings, "demo-host");
```
