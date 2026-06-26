# ContainerVolume

Maps a Windows path into the container.

```csharp
public sealed class ContainerVolume
{
    public ContainerVolume(string windowsPath, string containerPath, bool readOnly);

    public string WindowsPath { get; set; }
    public string ContainerPath { get; set; }
    public bool ReadOnly { get; set; }
}
```

Example:

```csharp
var volume = new ContainerVolume(@"C:\data", "/workspace/data", readOnly: false);
```
