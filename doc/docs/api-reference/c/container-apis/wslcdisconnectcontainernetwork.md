# WslcDisconnectContainerNetwork

```c
STDAPI WslcDisconnectContainerNetwork(_In_ WslcContainer container, _In_z_ PCSTR nameOrId, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `container` | `WslcContainer` | in |
| `nameOrId` | `PCSTR` | in |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Disconnects a created or running container from a session network. `nameOrId` is the name, full ID, or unique ID prefix of the network to disconnect from.

Returns `WSLC_E_NETWORK_NOT_FOUND` if no network matches, or `WSLC_E_NETWORK_PREFIX_AMBIGUOUS` if an ID prefix matches more than one network. Returns `E_INVALIDARG` if `nameOrId` is null or empty.

Notes:

- Requires WSL 3.1.0 or later; returns [`WSLC_E_RUNTIME_UPDATE_NEEDED`](../error-codes.md) on older runtimes.

Example:

```c
HRESULT hr = WslcDisconnectContainerNetwork(container, "backend", NULL);
```
