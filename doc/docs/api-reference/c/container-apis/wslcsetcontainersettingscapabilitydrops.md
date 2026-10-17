# WslcSetContainerSettingsCapabilityDrops

```c
STDAPI WslcSetContainerSettingsCapabilityDrops(
    _In_ WslcContainerSettings* containerSettings,
    _In_reads_opt_(capabilityCount) PCSTR const* capabilities,
    _In_ uint32_t capabilityCount);
```

| Parameter | Type | Direction |
|---|---|---|
| `containerSettings` | `WslcContainerSettings*` | in |
| `capabilities` | `PCSTR const*` | in, optional |
| `capabilityCount` | `uint32_t` | in |

Return value: `HRESULT`.

Example:

```c
const char* capabilities[] = { "NET_RAW", "CHOWN" };

HRESULT hr = WslcSetContainerSettingsCapabilityDrops(
    &containerSettings,
    capabilities,
    (uint32_t)_countof(capabilities));
```