# ContainerNetwork

Connects a container to a network.

```csharp
public sealed class ContainerNetwork
{
    public ContainerNetwork(string name);

    public string Name { get; set; }
    public IList<string> Aliases { get; set; }
    public HostName IPv4Address { get; set; }
    public HostName IPv6Address { get; set; }
}
```

Notes:

- `Name` must be non-empty.
- `Aliases`, `IPv4Address` and `IPv6Address` are only supported on user-defined networks (not `bridge`, `host`, `none`, or `container:<name>`).
- `IPv4Address` must be an IPv4 address and `IPv6Address` must be an IPv6 address. `null` lets the network assign one. `IPv6Address` requires a network with IPv6 enabled.

Example:

```csharp
var network = new ContainerNetwork("frontend")
{
    Aliases = new List<string> { "web" },
    IPv4Address = new HostName("10.0.0.10")
};
```
