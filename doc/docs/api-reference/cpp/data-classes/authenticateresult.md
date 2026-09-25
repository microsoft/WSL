# AuthenticateResult

Registry authentication data returned by `Session::Authenticate`.

**Properties**

| Property | Type |
|---|---|
| `IdentityToken()` | `hstring` |
| `TokenType()` | `IdentityTokenType` |

`IdentityToken()` contains opaque registry authentication data suitable for
`PullImageOptions::RegistryAuth()` or `PushImageOptions::RegistryAuth()`. `TokenType()` identifies
whether it represents an identity token or the supplied credentials.

```cpp
auto authentication = session.Authenticate(registryUri, username, password);
pullOptions.RegistryAuth(authentication.IdentityToken());
```
