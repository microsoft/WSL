# ImageInfo

Metadata returned by `Session.Images`.

```csharp
using Windows.Storage.Streams;

public sealed class ImageInfo
{
    public string Name { get; }
    public IBuffer Sha256 { get; }
    public ulong SizeBytes { get; }
    public DateTimeOffset CreatedTimestamp { get; }
}
```

Example:

```csharp
foreach (var image in session.Images)
{
    Console.WriteLine($"{image.Name} ({image.SizeBytes / 1024 / 1024} MB)");
}
```
