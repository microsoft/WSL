# ContainerNamedVolume

Maps a session-managed named VHD volume into the container.

```csharp
public sealed class ContainerNamedVolume
{
    public ContainerNamedVolume(string name, string containerPath, bool readOnly);

    public string Name { get; set; }
    public string ContainerPath { get; set; }
    public bool ReadOnly { get; set; }
}
```

Example:

```csharp
var namedVolume = new ContainerNamedVolume("cache", "/var/cache/app", readOnly: false);
```
