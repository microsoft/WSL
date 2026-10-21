# ContainerNetwork

Connects a container to a network.

```csharp
public sealed class ContainerNetwork
{
    public ContainerNetwork(string name);

    public string Name { get; set; }
    public IList<string> Aliases { get; set; }
}
```

Notes:

- `Name` must be non-empty.
- `Aliases` are only supported on user-defined networks (not `bridge`, `host`, `none`, or `container:<name>`).

Example:

```csharp
var network = new ContainerNetwork("frontend")
{
    Aliases = new List<string> { "web" }
};
```
