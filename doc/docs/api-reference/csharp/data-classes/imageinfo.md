# ImageInfo

Metadata returned by `Session.GetImages()`.

```csharp
using Windows.Storage.Streams;

public sealed class ImageInfo
{
    public string Name { get; }
    public IBuffer Sha256 { get; }
    public ulong Size { get; }
    public DateTimeOffset CreatedTimestamp { get; }
}
```

Example:

```csharp
foreach (var image in session.GetImages())
{
    Console.WriteLine($"{image.Name} ({image.Size / 1024 / 1024} MB)");
}
```
