# ContainerNetwork

Connects a container to a network.

```csharp
public sealed class ContainerNetwork
{
    public ContainerNetwork(string name);

    public string Name { get; set; }
    public IList<string> Aliases { get; set; }
    public HostName IPAddress { get; set; }
}
```

Notes:

- `Name` must be non-empty.
- `Aliases` and `IPAddress` are only supported on user-defined networks (not `bridge`, `host`, `none`, or `container:<name>`).
- `IPAddress` must be an IPv4 address. `null` lets the network assign one.

Example:

```csharp
var network = new ContainerNetwork("frontend")
{
    Aliases = new List<string> { "web" },
    IPAddress = new HostName("10.0.0.10")
};
```
