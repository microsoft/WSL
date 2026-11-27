# WslcConnectContainerNetwork

```c
STDAPI WslcConnectContainerNetwork(_In_ WslcContainer container, _In_ const WslcContainerNetwork* network, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `container` | `WslcContainer` | in |
| `network` | [`const WslcContainerNetwork*`](../structures/wslccontainernetwork.md) | in |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Connects a created or running container to a session network. `network->name` is the network name; a full network ID or unique ID prefix is also accepted.

Returns `WSLC_E_NETWORK_NOT_FOUND` if no network matches, or `WSLC_E_NETWORK_PREFIX_AMBIGUOUS` if an ID prefix matches more than one network. Returns `E_INVALIDARG` if the network name is null or empty, `aliases` and `aliasCount` disagree on being empty or an alias is null, or the container's networking mode does not support additional networks.

Notes:

- Aliases, `ipv4Address` and `ipv6Address` are only supported on user-defined networks (not `bridge`, `host`, `none`, or `container:<name>`).
- Requires WSL 3.1.0 or later; returns [`WSLC_E_RUNTIME_UPDATE_NEEDED`](../error-codes.md) on older runtimes.

Example:

```c
PCSTR aliases[] = { "web" };

WslcContainerNetwork network = { 0 };
network.name = "backend";
network.aliases = aliases;
network.aliasCount = (uint32_t)_countof(aliases);

HRESULT hr = WslcConnectContainerNetwork(container, &network, NULL);
```
