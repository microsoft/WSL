# ContainerNetwork

Connects a container to a network.

**Constructors / properties**

- `ContainerNetwork(hstring name)`
  - `name` must be non-empty.
- `Name()` / setter
- `Aliases()` / setter
  - Only supported on user-defined networks (not `bridge`, `host`, `none`, or `container:<name>`).
- `IPv4Address()` / setter
  - Static IPv4 address on this network. Only supported on user-defined networks. `nullptr` lets the network assign one.
- `IPv6Address()` / setter
  - Static IPv6 address on this network. Only supported on user-defined networks with IPv6 enabled. `nullptr` lets the network assign one.

```cpp
ContainerNetwork network{ L"frontend" };
network.Aliases().Append(L"web");
network.IPv4Address(winrt::Windows::Networking::HostName{ L"10.0.0.10" });
```
