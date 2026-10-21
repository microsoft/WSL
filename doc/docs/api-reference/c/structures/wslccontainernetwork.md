# WslcContainerNetwork

```c
typedef struct WslcContainerNetwork
{
    _In_z_ PCSTR name;                               // Network name (e.g. "bridge" or a network created with WslcCreateSessionNetwork)
    _In_reads_opt_(aliasCount) const PCSTR* aliases; // DNS aliases for the container on this network; user-defined networks only
    _In_ uint32_t aliasCount;
} WslcContainerNetwork;
```

| Field | Type |
|---|---|
| `name` | `PCSTR` |
| `aliases` | `const PCSTR*` |
| `aliasCount` | `uint32_t` |

Used with [WslcSetContainerSettingsNetworks](../container-apis/wslcsetcontainersettingsnetworks.md).
