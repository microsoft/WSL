# AuthenticateResult

Registry authentication data returned by `Session.Authenticate`.

```csharp
public sealed class AuthenticateResult
{
    public string IdentityToken { get; }
    public IdentityTokenType TokenType { get; }
}
```

`IdentityToken` is a base64-encoded JSON value that can be assigned directly to
`PullImageOptions.RegistryAuth` or `PushImageOptions.RegistryAuth`.

```csharp
AuthenticateResult authentication = session.Authenticate(registryUri, username, password);
pullOptions.RegistryAuth = authentication.IdentityToken;
```
