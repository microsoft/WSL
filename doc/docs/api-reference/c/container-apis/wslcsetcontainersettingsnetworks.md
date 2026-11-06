# WslcSetContainerSettingsNetworks

```c
STDAPI WslcSetContainerSettingsNetworks(
    _In_ WslcContainerSettings* containerSettings,
    _In_reads_opt_(networkCount) const WslcContainerNetwork* networks,
    _In_ uint32_t networkCount);
```

| Parameter | Type | Direction |
|---|---|---|
| `containerSettings` | `WslcContainerSettings*` | in |
| `networks` | `const WslcContainerNetwork*` | in, optional |
| `networkCount` | `uint32_t` | in |

Return value: `HRESULT`. Returns `E_INVALIDARG` if `networks` and `networkCount` disagree on being empty, a network name is null or empty, or `aliases` and `aliasCount` disagree on being empty or an alias is null.

Notes:

- The first network is the primary network.
- Cannot be combined with [WslcSetContainerSettingsNetworkingMode](wslcsetcontainersettingsnetworkingmode.md); [WslcCreateContainer](wslccreatecontainer.md) returns `E_INVALIDARG` if both are set.
- Aliases, `ipv4Address` and `ipv6Address` are only supported on user-defined networks (not `bridge`, `host`, `none`, or `container:<name>`); [WslcCreateContainer](wslccreatecontainer.md) returns `E_INVALIDARG` otherwise.
- Each network can have its own `ipv4Address` and `ipv6Address`. Set either to `NULL` to let the network assign one. An `ipv6Address` requires a network with IPv6 enabled.
- The `networks` array and the strings and addresses it references are not copied and must stay valid until `WslcCreateContainer` returns.
- Pass `NULL` and `0` to clear.

Example:

```c
PCSTR aliases[] = { "web" };
IN_ADDR backendIp = { 0 };
inet_pton(AF_INET, "10.0.1.10", &backendIp);

WslcContainerNetwork networks[2] = { 0 };
networks[0].name = "frontend";
networks[0].aliases = aliases;
networks[0].aliasCount = (uint32_t)_countof(aliases);
networks[1].name = "backend";
networks[1].ipv4Address = &backendIp;

HRESULT hr = WslcSetContainerSettingsNetworks(
    &containerSettings,
    networks,
    (uint32_t)_countof(networks));
```
