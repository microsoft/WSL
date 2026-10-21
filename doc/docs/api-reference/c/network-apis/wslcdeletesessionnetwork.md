# WslcDeleteSessionNetwork

```c
STDAPI WslcDeleteSessionNetwork(_In_ WslcSession session, _In_z_ PCSTR name, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `name` | `PCSTR` | in |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Returns `WSLC_E_NETWORK_NOT_FOUND` if the network does not exist, or `HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)` if a container is connected to the network. Returns [`WSLC_E_RUNTIME_UPDATE_NEEDED`](../error-codes.md) if the installed WSL runtime does not support network management.

Example:

```c
HRESULT hr = WslcDeleteSessionNetwork(session, "backend", NULL);
```
