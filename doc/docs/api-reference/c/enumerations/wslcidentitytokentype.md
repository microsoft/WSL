# WslcIdentityTokenType

Identifies the credential representation returned by
[`WslcSessionAuthenticate`](../session-apis/wslcsessionauthenticate.md).

```c
typedef enum WslcIdentityTokenType
{
    WSLC_IDENTITY_TOKEN_TYPE_UNKNOWN = 0,
    WSLC_IDENTITY_TOKEN_TYPE_TOKEN = 1,
    WSLC_IDENTITY_TOKEN_TYPE_CREDENTIALS = 2,
} WslcIdentityTokenType;
```

| Enumerator | Meaning |
|---|---|
| `WSLC_IDENTITY_TOKEN_TYPE_UNKNOWN` | No valid credential was returned. |
| `WSLC_IDENTITY_TOKEN_TYPE_TOKEN` | The encoded JSON contains an `identitytoken`. |
| `WSLC_IDENTITY_TOKEN_TYPE_CREDENTIALS` | The encoded JSON contains the supplied username and password. |
