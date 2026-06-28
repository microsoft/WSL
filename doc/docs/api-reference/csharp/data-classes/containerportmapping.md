# ContainerPortMapping

Represents a published port.

```csharp
using Windows.Networking;

public sealed class ContainerPortMapping
{
    public ContainerPortMapping(ushort windowsPort, ushort containerPort, PortProtocol protocol);

    public ushort WindowsPort { get; set; }
    public ushort ContainerPort { get; set; }
    public PortProtocol Protocol { get; set; }
    public HostName WindowsAddress { get; set; }
}
```

Notes:

- `WindowsAddress` **is implemented**.
- It accepts only `HostNameType.Ipv4` and `HostNameType.Ipv6` values.
- DNS names are rejected.
- `null` means the default host bind address.

Example:

```csharp
using Windows.Networking;

var mapping = new ContainerPortMapping(8080, 80, PortProtocol.TCP)
{
    WindowsAddress = new HostName("127.0.0.1")
};
```
