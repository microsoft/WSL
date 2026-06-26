# WslcSessionAuthenticate

```c
STDAPI WslcSessionAuthenticate(
    _In_ WslcSession session,
    _In_z_ PCSTR serverAddress,
    _In_z_ PCSTR username,
    _In_z_ PCSTR password,
    _Outptr_result_z_ PSTR* identityToken,
    _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `serverAddress` | `PCSTR` | in |
| `username` | `PCSTR` | in |
| `password` | `PCSTR` | in |
| `identityToken` | `PSTR*` | out |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Header note: `identityToken` is allocated using `CoTaskMemAlloc`; free it with `CoTaskMemFree`.

Example:

```c
PSTR identityToken = NULL;
HRESULT hr = WslcSessionAuthenticate(
    session,
    "127.0.0.1:5000",
    "user",
    "password",
    &identityToken,
    NULL);

if (SUCCEEDED(hr))
{
    printf("token=%s\n", identityToken);
    CoTaskMemFree(identityToken);
}
```

---
