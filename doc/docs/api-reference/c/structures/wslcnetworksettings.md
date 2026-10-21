# WslcNetworkSettings

```c
typedef struct WslcNetworkSettings
{
    _In_z_ PCSTR name;
    WslcNetworkFlags flags;
    uint8_t ipv4PrefixLength;
    uint8_t ipv6PrefixLength;
    IN_ADDR ipv4Subnet;
    IN_ADDR ipv4Gateway;
    IN6_ADDR ipv6Subnet;
    IN6_ADDR ipv6Gateway;
} WslcNetworkSettings;
```

| Field | Type | Notes |
|---|---|---|
| `name` | `PCSTR` | Network name. |
| `flags` | `WslcNetworkFlags` | Only `WSLC_NETWORK_FLAG_ENABLE_IPV6` and `WSLC_NETWORK_FLAG_INTERNAL` are accepted. IPv4 is always enabled. |
| `ipv4PrefixLength` | `uint8_t` | IPv4 subnet prefix length (0-32). 0 means no IPv4 subnet. |
| `ipv6PrefixLength` | `uint8_t` | IPv6 subnet prefix length (0-128). 0 means no IPv6 subnet. Requires `WSLC_NETWORK_FLAG_ENABLE_IPV6`. |
| `ipv4Subnet` | `IN_ADDR` | IPv4 subnet address, used when `ipv4PrefixLength` is not 0. |
| `ipv4Gateway` | `IN_ADDR` | Optional IPv4 gateway. All zeros lets the session choose. |
| `ipv6Subnet` | `IN6_ADDR` | IPv6 subnet address, used when `ipv6PrefixLength` is not 0. |
| `ipv6Gateway` | `IN6_ADDR` | Optional IPv6 gateway. All zeros lets the session choose. |

At most one of `ipv4PrefixLength` and `ipv6PrefixLength` may be set. When both are 0, the session chooses an IPv4 subnet. Addresses are in network byte order.
