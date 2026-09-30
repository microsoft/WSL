# WslcContainerPortMapping

```c
typedef struct WslcContainerPortMapping
{
    _In_ uint16_t windowsPort;      // Port on Windows host
    _In_ uint16_t containerPort;    // Port inside container
    _In_ WslcPortProtocol protocol; // TCP or UDP

    // if you want to override the default binding address
    _In_opt_ struct sockaddr_storage* windowsAddress; // accepts ipv4/6
} WslcContainerPortMapping;
```

| Field | Type |
|---|---|
| `windowsPort` | `uint16_t` |
| `containerPort` | `uint16_t` |
| `protocol` | `WslcPortProtocol` |
| `windowsAddress` | `struct sockaddr_storage*` |
