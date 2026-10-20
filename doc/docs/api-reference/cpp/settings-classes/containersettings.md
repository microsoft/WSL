# ContainerSettings

**Constructor**

- `ContainerSettings(hstring imageName)`
  - `imageName` must be non-empty.

**Properties**

- `ImageName()` / setter
- `Name()` / setter
- `InitProcess()` / setter
- `NetworkingMode()` / setter (`None` and `Bridged` only)
- `HostName()` / setter
- `DomainName()` / setter
- `EnableAutoRemove()` / setter
- `EnableGpu()` / setter
- `Privileged()` / setter
- `PortMappings()` / setter
- `Volumes()` / setter
- `NamedVolumes()` / setter

**Important notes**

- Collection setters reject `nullptr`.
- When converting to the C struct, null elements inside the collections are rejected.

```cpp
using namespace winrt::Windows::Foundation::Collections;

ContainerSettings containerSettings{ L"demo-image:latest" };
containerSettings.Name(L"demo-container");
containerSettings.NetworkingMode(
    winrt::box_value(ContainerNetworkingMode::Bridged)
        .as<winrt::Windows::Foundation::IReference<ContainerNetworkingMode>>());
containerSettings.HostName(L"demo-host");
containerSettings.DomainName(L"localdomain");
containerSettings.EnableAutoRemove(false);
containerSettings.EnableGpu(false);
containerSettings.Privileged(false);

auto ports = single_threaded_vector<ContainerPortMapping>();
ports.Append(ContainerPortMapping{ 8080, 80, PortProtocol::TCP });
containerSettings.PortMappings(ports);

auto volumes = single_threaded_vector<ContainerVolume>();
volumes.Append(ContainerVolume{ L"C:\\src", L"/src", false });
containerSettings.Volumes(volumes);

auto namedVolumes = single_threaded_vector<ContainerNamedVolume>();
namedVolumes.Append(ContainerNamedVolume{ L"cache", L"/cache", false });
containerSettings.NamedVolumes(namedVolumes);
```
