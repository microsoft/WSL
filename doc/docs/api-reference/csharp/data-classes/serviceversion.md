# ServiceVersion

WSL service version information.

```csharp
public sealed class ServiceVersion
{
    public uint Major { get; }
    public uint Minor { get; }
    public uint Revision { get; }
}
```

Example:

```csharp
var version = WslcService.GetVersion();
Console.WriteLine($"WSL service: {version.Major}.{version.Minor}.{version.Revision}");
```

---
