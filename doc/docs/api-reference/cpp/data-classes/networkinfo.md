# NetworkInfo

Read-only wrapper created from `WslcNetworkInfo`.

**Properties**

- `Id()` → network ID
- `Name()` → network name
- `Driver()` → network driver
- `CreatedTimestamp()` → WinRT `DateTime`
- `Flags()` → [`NetworkFlags`](../enumerations/networkflags.md)
- `IPv4Subnet()` → IPv4 subnet address as `Windows::Networking::HostName`, null if none
- `IPv4SubnetPrefixLength()` → IPv4 subnet prefix length, 0 if none
- `IPv4Gateway()` → IPv4 gateway as `Windows::Networking::HostName`, null if none
- `IPv6Subnet()` → IPv6 subnet address as `Windows::Networking::HostName`, null if none
- `IPv6SubnetPrefixLength()` → IPv6 subnet prefix length, 0 if none
- `IPv6Gateway()` → IPv6 gateway as `Windows::Networking::HostName`, null if none

```cpp
auto networks = session.GetNetworks();
for (auto const& network : networks)
{
    printf("%ws %ws\n", network.Name().c_str(), network.Driver().c_str());
    if (auto subnet = network.IPv4Subnet())
    {
        printf("  %ws/%u\n", subnet.CanonicalName().c_str(), network.IPv4SubnetPrefixLength());
    }
}
```