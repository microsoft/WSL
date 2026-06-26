# PullImageOptions

```csharp
public sealed class PullImageOptions
{
    public PullImageOptions(string uri);

    public string Uri { get; set; }
    public string RegistryAuth { get; set; }
}
```

Example:

```csharp
var pullOptions = new PullImageOptions("docker.io/library/alpine:latest")
{
    RegistryAuth = string.Empty // optional for public registries
};
```
