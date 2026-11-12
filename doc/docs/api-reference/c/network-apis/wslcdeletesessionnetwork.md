# WslcDeleteSessionNetwork

```c
STDAPI WslcDeleteSessionNetwork(_In_ WslcSession session, _In_z_ PCSTR nameOrId, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `nameOrId` | `PCSTR` | in |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

`nameOrId` is the name, full ID, or unique ID prefix of the network to delete. An exact ID match takes precedence over a name match, and a name match takes precedence over an ID prefix match.

Returns `WSLC_E_NETWORK_NOT_FOUND` if no network matches, `WSLC_E_NETWORK_PREFIX_AMBIGUOUS` if an ID prefix matches more than one network, or `HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)` if a container is connected to the network. Returns [`WSLC_E_RUNTIME_UPDATE_NEEDED`](../error-codes.md) if the installed WSL runtime does not support network management.

Example:

```c
HRESULT hr = WslcDeleteSessionNetwork(session, "backend", NULL);
```
