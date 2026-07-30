# ContainerPortMapping

Maps a Windows host port to a container port.

**Constructors / properties**

- `ContainerPortMapping(uint16_t windowsPort, uint16_t containerPort, PortProtocol protocol)`
- `WindowsPort()` / setter
- `ContainerPort()` / setter
- `Protocol()` / setter
- `WindowsAddress()` / setter

**Important notes**

- `WindowsAddress` is implemented.
- The setter accepts only `Windows::Networking::HostName` values whose type is `Ipv4` or `Ipv6`.
- `ToStruct()` uses `inet_pton` and stores a real `sockaddr_in` / `sockaddr_in6`.

```cpp
using namespace winrt::Windows::Networking;

ContainerPortMapping mapping{ 8080, 80, PortProtocol::TCP };
mapping.WindowsAddress(HostName{ L"127.0.0.1" });

auto hostPort = mapping.WindowsPort();
auto guestPort = mapping.ContainerPort();
auto protocol = mapping.Protocol();
auto bindAddress = mapping.WindowsAddress();
```
