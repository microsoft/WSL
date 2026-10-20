# ContainerSettings

Configures a container before creation.

```csharp
public sealed class ContainerSettings
{
    public ContainerSettings(string imageName);

    public string ImageName { get; set; }
    public string Name { get; set; }
    public ProcessSettings InitProcess { get; set; }
    public ContainerNetworkingMode? NetworkingMode { get; set; }
    public string HostName { get; set; }
    public string DomainName { get; set; }
    public bool EnableAutoRemove { get; set; }
    public bool EnableGpu { get; set; }
    public bool Privileged { get; set; }
    public IList<ContainerPortMapping> PortMappings { get; set; }
    public IList<ContainerVolume> Volumes { get; set; }
    public IList<ContainerNamedVolume> NamedVolumes { get; set; }
}
```

Notes:

- `PortMappings`, `Volumes`, and `NamedVolumes` are mutable collections.
- `InitProcess` is optional.
- `NetworkingMode` is nullable; `null` means “leave default behavior”.

Example:

```csharp
var init = new ProcessSettings
{
    CommandLine = new List<string> { "/bin/sh", "-c", "echo hello from init" },
    OutputMode = ProcessOutputMode.Event
};

var containerSettings = new ContainerSettings("docker.io/library/alpine:latest")
{
    Name = "demo-container",
    InitProcess = init,
    NetworkingMode = ContainerNetworkingMode.Bridged,
    EnableAutoRemove = true,
    PortMappings = new List<ContainerPortMapping>
    {
        new(8080, 80, PortProtocol.TCP)
    },
    Volumes = new List<ContainerVolume>
    {
        new(@"C:\data", "/workspace/data", false)
    },
    NamedVolumes = new List<ContainerNamedVolume>
    {
        new("cache", "/var/cache/app", false)
    }
};
```
