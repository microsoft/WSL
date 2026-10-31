# AuthenticateResult

Registry authentication data returned by `Session.Authenticate`.

```csharp
public sealed class AuthenticateResult
{
    public string IdentityToken { get; }
    public IdentityTokenType TokenType { get; }
}
```

`IdentityToken` contains opaque registry authentication data that can be assigned directly to
`PullImageOptions.RegistryAuth` or `PushImageOptions.RegistryAuth`. `TokenType` identifies whether
it represents an identity token or the supplied credentials.

```csharp
AuthenticateResult authentication = session.Authenticate(registryUri, username, password);
pullOptions.RegistryAuth = authentication.IdentityToken;
```
