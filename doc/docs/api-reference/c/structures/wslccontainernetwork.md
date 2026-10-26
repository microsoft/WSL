# WslcContainerNetwork

```c
typedef struct WslcContainerNetwork
{
    _In_z_ PCSTR name;                               // Network name (e.g. "bridge" or a network created with WslcCreateSessionNetwork)
    _In_reads_opt_(aliasCount) const PCSTR* aliases; // DNS aliases for the container on this network; user-defined networks only
    _In_ uint32_t aliasCount;
    _In_opt_ const IN_ADDR* ipv4Address;             // Static IPv4 address for the container on this network; user-defined networks only
    _In_opt_ const IN6_ADDR* ipv6Address;            // Static IPv6 address for the container on this network; user-defined networks only
} WslcContainerNetwork;
```

| Field | Type |
|---|---|
| `name` | `PCSTR` |
| `aliases` | `const PCSTR*` |
| `aliasCount` | `uint32_t` |
| `ipv4Address` | `const IN_ADDR*` |
| `ipv6Address` | `const IN6_ADDR*` |

Used with [WslcSetContainerSettingsNetworks](../container-apis/wslcsetcontainersettingsnetworks.md).
