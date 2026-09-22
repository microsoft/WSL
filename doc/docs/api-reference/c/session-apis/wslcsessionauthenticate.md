# WslcSessionAuthenticate

```c
STDAPI WslcSessionAuthenticate(
    _In_ WslcSession session,
    _In_z_ PCSTR serverAddress,
    _In_z_ PCSTR username,
    _In_z_ PCSTR password,
    _Outptr_result_z_ PSTR* identityToken,
    _Out_opt_ WslcIdentityTokenType* tokenType,
    _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `serverAddress` | `PCSTR` | in |
| `username` | `PCSTR` | in |
| `password` | `PCSTR` | in |
| `identityToken` | `PSTR*` | out |
| `tokenType` | `WslcIdentityTokenType*` | out, optional |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

`identityToken` is a base64-encoded JSON value suitable for the `registryAuth` field of
`WslcPullImageOptions` or `WslcPushImageOptions`. It contains either an `identitytoken` returned by
the registry or the supplied username and password.

`tokenType` reports which representation is encoded. After the input arguments and session are
validated, it is initialized to `WSLC_IDENTITY_TOKEN_TYPE_UNKNOWN` and retains that value if
authentication fails. If input validation fails, its value is unchanged.

`identityToken` is allocated using `CoTaskMemAlloc`; free it with `CoTaskMemFree`.

Example:

```c
PSTR identityToken = NULL;
WslcIdentityTokenType tokenType = WSLC_IDENTITY_TOKEN_TYPE_UNKNOWN;
HRESULT hr = WslcSessionAuthenticate(
    session,
    "127.0.0.1:5000",
    "user",
    "password",
    &identityToken,
    &tokenType,
    NULL);

if (SUCCEEDED(hr))
{
    printf("registry auth type=%u\n", (unsigned)tokenType);
    CoTaskMemFree(identityToken);
}
```
