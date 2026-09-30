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

`identityToken` contains opaque registry authentication data suitable for the `registryAuth` field
of `WslcPullImageOptions` or `WslcPushImageOptions`.

On success, `tokenType` reports whether the authentication data contains an identity token or the
supplied credentials.

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
