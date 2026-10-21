# NetworkSettings

```csharp
public sealed class NetworkSettings
{
    public NetworkSettings(string name);

    public string Name { get; set; }
    public NetworkFlags Flags { get; set; }
    public HostName Subnet { get; set; }
    public byte SubnetPrefixLength { get; set; }
    public HostName Gateway { get; set; }
}
```

Only `NetworkFlags.EnableIPv6` and `NetworkFlags.Internal` are accepted; IPv4 is always enabled. `Subnet` and `SubnetPrefixLength` must be set together, and `Gateway` must use the same address family as `Subnet`. An IPv6 subnet requires `NetworkFlags.EnableIPv6`. When `Subnet` is null, the session chooses an IPv4 subnet.

Example:

```csharp
var settings = new NetworkSettings("backend")
{
    Flags = NetworkFlags.Internal,
    Subnet = new HostName("10.10.0.0"),
    SubnetPrefixLength = 24,
    Gateway = new HostName("10.10.0.1"),
};
```
