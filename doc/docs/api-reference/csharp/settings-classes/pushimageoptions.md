# PushImageOptions

```csharp
public sealed class PushImageOptions
{
    public PushImageOptions(string image, string registryAuth);

    public string Image { get; set; }
    public string RegistryAuth { get; set; }
}
```

Example:

```csharp
var pushOptions = new PushImageOptions("registry.example.com/demo:latest", authToken);
```
