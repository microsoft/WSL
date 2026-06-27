# ContainerNamedVolume

Attaches a named session VHD volume to a container.

**Constructors / properties**

- `ContainerNamedVolume(hstring name, hstring containerPath, bool readOnly)`
- `Name()` / setter
- `ContainerPath()` / setter
- `ReadOnly()` / setter

```cpp
ContainerNamedVolume named{ L"build-cache", L"/cache", false };
named.Name(L"build-cache");
named.ContainerPath(L"/cache");
named.ReadOnly(false);
```
