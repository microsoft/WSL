# IdentityTokenType

Describes the credential encoded in `AuthenticateResult::IdentityToken()`.

```cpp
enum class IdentityTokenType
{
    Unknown = 0,
    Token = 1,
    Credentials = 2
};
```

| Value | Meaning |
|---|---|
| `Unknown` | No valid credential was returned. |
| `Token` | The encoded JSON contains an `identitytoken`. |
| `Credentials` | The encoded JSON contains the supplied username and password. |
