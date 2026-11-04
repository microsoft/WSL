# WslcSetContainerSettingsNetworkingMode

```c
STDAPI WslcSetContainerSettingsNetworkingMode(_In_ WslcContainerSettings* containerSettings, _In_ WslcContainerNetworkingMode networkingMode);
```

| Parameter | Type | Direction |
|---|---|---|
| `containerSettings` | `WslcContainerSettings*` | in |
| `networkingMode` | `WslcContainerNetworkingMode` | in |

Return value: `HRESULT`.

Notes:

- Defaults to `WSLC_CONTAINER_NETWORKING_MODE_NONE` when neither this nor [WslcSetContainerSettingsNetworks](wslcsetcontainersettingsnetworks.md) is called.
- Cannot be combined with [WslcSetContainerSettingsNetworks](wslcsetcontainersettingsnetworks.md); [WslcCreateContainer](wslccreatecontainer.md) returns `E_INVALIDARG` if both are set.

Example:

```c
HRESULT hr = WslcSetContainerSettingsNetworkingMode(
    &containerSettings,
    WSLC_CONTAINER_NETWORKING_MODE_BRIDGED);
```
