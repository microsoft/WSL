# WslcSetContainerSettingsNamedVolumes

```c
STDAPI WslcSetContainerSettingsNamedVolumes(
    _In_ WslcContainerSettings* containerSettings,
    _In_reads_opt_(namedVolumeCount) const WslcContainerNamedVolume* namedVolumes,
    _In_ uint32_t namedVolumeCount);
```

| Parameter | Type | Direction |
|---|---|---|
| `containerSettings` | `WslcContainerSettings*` | in |
| `namedVolumes` | `const WslcContainerNamedVolume*` | in, optional |
| `namedVolumeCount` | `uint32_t` | in |

Return value: `HRESULT`.

Example:

```c
WslcContainerNamedVolume namedVolumes[1] = { 0 };
namedVolumes[0].name = "cache";
namedVolumes[0].containerPath = "/var/cache/demo";
namedVolumes[0].readOnly = FALSE;

HRESULT hr = WslcSetContainerSettingsNamedVolumes(
    &containerSettings,
    namedVolumes,
    (uint32_t)_countof(namedVolumes));
```
