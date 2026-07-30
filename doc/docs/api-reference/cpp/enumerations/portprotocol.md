# PortProtocol

- `TCP` is the default in `winrt_ContainerPortMapping.h`.
- The value is passed directly to `WslcContainerPortMapping::protocol`.

Underlying C values:

- `TCP = 0`
- `UDP = 1`

```cpp
ContainerPortMapping mapping{ 8080, 80, PortProtocol::TCP };
```
