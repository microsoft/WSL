# NetworkSettings

**Constructor**

- `NetworkSettings(hstring name)`

**Properties**

| Property | Type |
|---|---|
| `Name()` / setter | `hstring` |
| `Flags()` / setter | `NetworkFlags` |
| `Subnet()` / setter | `Windows::Networking::HostName` |
| `SubnetPrefixLength()` / setter | `uint8_t` |
| `Gateway()` / setter | `Windows::Networking::HostName` |

Only `NetworkFlags::EnableIPv6` and `NetworkFlags::Internal` are accepted; IPv4 is always enabled. `Subnet` and `SubnetPrefixLength` must be set together, and `Gateway` must use the same address family as `Subnet`. An IPv6 subnet requires `NetworkFlags::EnableIPv6`. When `Subnet` is null, the session chooses an IPv4 subnet.

```cpp
NetworkSettings settings{L"backend"};
settings.Flags(NetworkFlags::Internal);
settings.Subnet(Windows::Networking::HostName{L"10.10.0.0"});
settings.SubnetPrefixLength(24);
settings.Gateway(Windows::Networking::HostName{L"10.10.0.1"});
auto network = session.CreateNetwork(settings);
```
