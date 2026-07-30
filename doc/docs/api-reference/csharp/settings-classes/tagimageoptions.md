# TagImageOptions

```csharp
public sealed class TagImageOptions
{
    public TagImageOptions(string image, string repository, string tag);

    public string Image { get; set; }
    public string Repository { get; set; }
    public string Tag { get; set; }
}
```

Example:

```csharp
var tagOptions = new TagImageOptions("alpine:latest", "registry.example.com/alpine", "v1");
```
