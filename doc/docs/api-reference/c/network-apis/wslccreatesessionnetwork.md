# WslcCreateSessionNetwork

```c
STDAPI WslcCreateSessionNetwork(
    _In_ WslcSession session, _In_ const WslcNetworkSettings* settings, _Out_opt_ WslcNetworkInfo* networkInfo, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `settings` | `const WslcNetworkSettings*` | in |
| `networkInfo` | `WslcNetworkInfo*` | out, optional |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Creates a bridge network in the session. When `networkInfo` is provided, it receives the created network.

Returns `HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)` if a network with the same name exists. Returns `E_INVALIDARG` if the name is invalid or reserved, `flags` contains anything other than `WSLC_NETWORK_FLAG_ENABLE_IPV4`, `WSLC_NETWORK_FLAG_ENABLE_IPV6` and `WSLC_NETWORK_FLAG_INTERNAL`, both prefix lengths are set, a prefix length is out of range, an IPv6 subnet is set without `WSLC_NETWORK_FLAG_ENABLE_IPV6`, or a gateway is set without a subnet of the same family. Returns `E_NOTIMPL` if `WSLC_NETWORK_FLAG_ENABLE_IPV6` is set without `WSLC_NETWORK_FLAG_ENABLE_IPV4`; IPv4 cannot be disabled, so set both flags for a dual-stack network. Returns [`WSLC_E_RUNTIME_UPDATE_NEEDED`](../error-codes.md) if the installed WSL runtime does not support network management.

Example:

```c
WslcNetworkSettings settings = {0};
settings.name = "backend";
settings.flags = WSLC_NETWORK_FLAG_INTERNAL;
settings.ipv4PrefixLength = 24;
inet_pton(AF_INET, "10.10.0.0", &settings.ipv4Subnet);
inet_pton(AF_INET, "10.10.0.1", &settings.ipv4Gateway);

WslcNetworkInfo network = {0};
HRESULT hr = WslcCreateSessionNetwork(session, &settings, &network, NULL);
```
