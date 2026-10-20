# NetworkInfo

Metadata returned by `Session.GetNetworks()`.

```csharp
public sealed class NetworkInfo
{
    public string Id { get; }
    public string Name { get; }
    public string Driver { get; }
    public string Scope { get; }
    public DateTimeOffset CreatedTimestamp { get; }
    public bool EnableIPv4 { get; }
    public bool EnableIPv6 { get; }
    public bool Internal { get; }
}
```

Example:

```csharp
foreach (var network in session.GetNetworks())
{
    Console.WriteLine($"{network.Name} ({network.Driver}, {network.Scope})");
}
```