# WslcSetContainerSettingsName

```c
STDAPI WslcSetContainerSettingsName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR name);
```

| Parameter | Type | Direction |
|---|---|---|
| `containerSettings` | `WslcContainerSettings*` | in |
| `name` | `PCSTR` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcSetContainerSettingsName(&containerSettings, "demo-container");
```
