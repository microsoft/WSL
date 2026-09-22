# AuthenticateResult

Registry authentication data returned by `Session::Authenticate`.

**Properties**

- `IdentityToken()`
- `TokenType()`

`IdentityToken()` is a base64-encoded JSON value suitable for
`PullImageOptions::RegistryAuth()` or `PushImageOptions::RegistryAuth()`.

```cpp
auto authentication = session.Authenticate(registryUri, username, password);
pullOptions.RegistryAuth(authentication.IdentityToken());
```
