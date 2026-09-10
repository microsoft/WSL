# ContainerVolume

Binds a Windows path into the container.

**Constructors / properties**

- `ContainerVolume(hstring windowsPath, hstring containerPath, bool readOnly)`
- `WindowsPath()` / setter
- `ContainerPath()` / setter
- `ReadOnly()` / setter

```cpp
ContainerVolume volume{ L"C:\\data", L"/workspace", false };
volume.ReadOnly(true);
volume.WindowsPath(L"C:\\data");
volume.ContainerPath(L"/workspace");
```
