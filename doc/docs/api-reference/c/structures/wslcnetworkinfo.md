# WslcNetworkInfo

```c
typedef struct WslcNetworkInfo
{
    CHAR id[WSLC_NETWORK_ID_LENGTH];
    CHAR name[WSLC_NETWORK_NAME_LENGTH];
    CHAR driver[WSLC_NETWORK_DRIVER_LENGTH];
    CHAR scope[WSLC_NETWORK_SCOPE_LENGTH];
    int64_t createdUnixTime;
    BOOL enableIPv4;
    BOOL enableIPv6;
    BOOL internal;
} WslcNetworkInfo;
```

| Field | Type |
|---|---|
| `id` | `CHAR[WSLC_NETWORK_ID_LENGTH]` |
| `name` | `CHAR[WSLC_NETWORK_NAME_LENGTH]` |
| `driver` | `CHAR[WSLC_NETWORK_DRIVER_LENGTH]` |
| `scope` | `CHAR[WSLC_NETWORK_SCOPE_LENGTH]` |
| `createdUnixTime` | `int64_t` (`0` if unknown) |
| `enableIPv4` | `BOOL` |
| `enableIPv6` | `BOOL` |
| `internal` | `BOOL` |