# WslcNetworkInfo

```c
typedef struct WslcNetworkInfo
{
    CHAR id[WSLC_NETWORK_ID_LENGTH];
    CHAR name[WSLC_NETWORK_NAME_LENGTH];
    CHAR driver[WSLC_NETWORK_DRIVER_LENGTH];
    int64_t createdUnixTime;
    WslcNetworkFlags flags;
    uint8_t ipv4PrefixLength;
    uint8_t ipv6PrefixLength;
    IN_ADDR ipv4Subnet;
    IN_ADDR ipv4Gateway;
    IN6_ADDR ipv6Subnet;
    IN6_ADDR ipv6Gateway;
} WslcNetworkInfo;
```

| Field | Type |
|---|---|
| `id` | `CHAR[WSLC_NETWORK_ID_LENGTH]` |
| `name` | `CHAR[WSLC_NETWORK_NAME_LENGTH]` |
| `driver` | `CHAR[WSLC_NETWORK_DRIVER_LENGTH]` |
| `createdUnixTime` | `int64_t` (`0` if unknown) |
| `flags` | [`WslcNetworkFlags`](../enumerations/wslcnetworkflags.md) |
| `ipv4PrefixLength` | `uint8_t` (`0` if the network has no IPv4 subnet) |
| `ipv6PrefixLength` | `uint8_t` (`0` if the network has no IPv6 subnet) |
| `ipv4Subnet` | `IN_ADDR` |
| `ipv4Gateway` | `IN_ADDR` (all zeros if the network has no IPv4 gateway) |
| `ipv6Subnet` | `IN6_ADDR` |
| `ipv6Gateway` | `IN6_ADDR` (all zeros if the network has no IPv6 gateway) |

Addresses are in network byte order. Only the first configured subnet of each address family is reported.