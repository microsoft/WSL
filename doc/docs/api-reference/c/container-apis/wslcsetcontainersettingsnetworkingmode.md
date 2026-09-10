# WslcSetContainerSettingsNetworkingMode

```c
STDAPI WslcSetContainerSettingsNetworkingMode(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerNetworkingMode networkingMode);
```

| Parameter | Type | Direction |
|---|---|---|
| `containerSettings` | `WslcContainerSettings*` | in |
| `networkingMode` | `WslcContainerNetworkingMode` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcSetContainerSettingsNetworkingMode(
    &containerSettings,
    WSLC_CONTAINER_NETWORKING_MODE_BRIDGED);
```
