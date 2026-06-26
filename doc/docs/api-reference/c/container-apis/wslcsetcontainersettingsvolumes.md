# WslcSetContainerSettingsVolumes

```c
STDAPI WslcSetContainerSettingsVolumes(
    _In_ WslcContainerSettings* containerSettings, _In_reads_opt_(volumeCount) const WslcContainerVolume* volumes, _In_ uint32_t volumeCount);
```

| Parameter | Type | Direction |
|---|---|---|
| `containerSettings` | `WslcContainerSettings*` | in |
| `volumes` | `const WslcContainerVolume*` | in, optional |
| `volumeCount` | `uint32_t` | in |

Return value: `HRESULT`.

Example:

```c
WslcContainerVolume volumes[1] = { 0 };
volumes[0].windowsPath = L"C:\\data";
volumes[0].containerPath = "/mnt/data";
volumes[0].readOnly = FALSE;

HRESULT hr = WslcSetContainerSettingsVolumes(
    &containerSettings,
    volumes,
    (uint32_t)_countof(volumes));
```
