# WslcListSessionNetworks

```c
STDAPI WslcListSessionNetworks(
    _In_ WslcSession session,
    _Outptr_result_buffer_maybenull_(*count) WslcNetworkInfo** networks,
    _Out_ uint32_t* count,
    _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `networks` | `WslcNetworkInfo**` | out |
| `count` | `uint32_t*` | out |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`. Returns [`WSLC_E_RUNTIME_UPDATE_NEEDED`](../error-codes.md) if the installed WSL runtime does not support network management.

`networks` is allocated using `CoTaskMemAlloc`; free it with `CoTaskMemFree`. `networks` is `NULL` when `count` is `0`.

Example:

```c
WslcNetworkInfo* networks = NULL;
uint32_t count = 0;
HRESULT hr = WslcListSessionNetworks(session, &networks, &count, NULL);

if (SUCCEEDED(hr))
{
    for (uint32_t i = 0; i < count; ++i)
    {
        printf("%s %s %s\n", networks[i].id, networks[i].name, networks[i].driver);
    }
    CoTaskMemFree(networks);
}
```
