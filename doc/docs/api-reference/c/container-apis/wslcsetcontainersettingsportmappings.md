# WslcSetContainerSettingsPortMappings

```c
STDAPI WslcSetContainerSettingsPortMappings(
    _In_ WslcContainerSettings* containerSettings,
    _In_reads_opt_(portMappingCount) const WslcContainerPortMapping* portMappings,
    _In_ uint32_t portMappingCount);
```

| Parameter | Type | Direction |
|---|---|---|
| `containerSettings` | `WslcContainerSettings*` | in |
| `portMappings` | `const WslcContainerPortMapping*` | in, optional |
| `portMappingCount` | `uint32_t` | in |

Return value: `HRESULT`.

Example:

```c
WslcContainerPortMapping portMappings[1] = { 0 };
portMappings[0].windowsPort = (uint16_t)8080;
portMappings[0].containerPort = (uint16_t)80;
portMappings[0].protocol = WSLC_PORT_PROTOCOL_TCP;
portMappings[0].windowsAddress = NULL;

HRESULT hr = WslcSetContainerSettingsPortMappings(
    &containerSettings,
    portMappings,
    (uint32_t)_countof(portMappings));
```
