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

`NetworkFlags::EnableIPv4`, `NetworkFlags::EnableIPv6` and `NetworkFlags::Internal` are accepted. IPv4 is always enabled. To enable IPv6, set both `NetworkFlags::EnableIPv4` and `NetworkFlags::EnableIPv6`; IPv6-only networks (`EnableIPv6` without `EnableIPv4`) are not supported and fail with `E_NOTIMPL`. `Subnet` and `SubnetPrefixLength` must be set together, `Gateway` must use the same address family as `Subnet`, and an IPv6 subnet requires `EnableIPv6`. When `Subnet` is null, the session chooses an IPv4 subnet; it has no default IPv6 subnets, so set an IPv6 subnet when IPv6 is enabled.

```cpp
NetworkSettings settings{L"backend"};
settings.Flags(NetworkFlags::Internal);
settings.Subnet(Windows::Networking::HostName{L"10.10.0.0"});
settings.SubnetPrefixLength(24);
settings.Gateway(Windows::Networking::HostName{L"10.10.0.1"});
auto network = session.CreateNetwork(settings);
```
