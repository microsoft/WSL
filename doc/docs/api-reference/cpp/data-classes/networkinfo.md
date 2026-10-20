# NetworkInfo

Read-only wrapper created from `WslcNetworkInfo`.

**Properties**

- `Id()` → network ID
- `Name()` → network name
- `Driver()` → network driver
- `Scope()` → network scope
- `CreatedTimestamp()` → WinRT `DateTime`
- `EnableIPv4()` → whether IPv4 is enabled
- `EnableIPv6()` → whether IPv6 is enabled
- `Internal()` → whether the network is internal

```cpp
auto networks = session.GetNetworks();
for (auto const& network : networks)
{
    printf("%ws %ws\n", network.Name().c_str(), network.Driver().c_str());
}
```