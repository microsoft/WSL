# NetworkInfo

Metadata returned by `Session.GetNetworks()`.

```csharp
public sealed class NetworkInfo
{
    public string Id { get; }
    public string Name { get; }
    public string Driver { get; }
    public DateTimeOffset CreatedTimestamp { get; }
    public NetworkFlags Flags { get; }
    public HostName IPv4Subnet { get; }
    public byte IPv4SubnetPrefixLength { get; }
    public HostName IPv4Gateway { get; }
    public HostName IPv6Subnet { get; }
    public byte IPv6SubnetPrefixLength { get; }
    public HostName IPv6Gateway { get; }
}
```

`Flags` is a [`NetworkFlags`](../enumerations/networkflags.md) value. Addresses are `Windows.Networking.HostName` values. Subnet and gateway properties are null and prefix lengths are 0 when the network has none for that address family.

Example:

```csharp
foreach (var network in session.GetNetworks())
{
    var subnet = network.IPv4Subnet is null ? "none" : $"{network.IPv4Subnet.CanonicalName}/{network.IPv4SubnetPrefixLength}";
    Console.WriteLine($"{network.Name} ({network.Driver}, {subnet})");
}
```