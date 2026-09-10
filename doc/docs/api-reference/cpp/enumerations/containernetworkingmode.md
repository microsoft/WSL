# ContainerNetworkingMode

`winrt_ContainerSettings.cpp` explicitly validates only:

- `None`
- `Bridged`

Underlying C values:

- `None = 0`
- `Bridged = 1`

```cpp
containerSettings.NetworkingMode(ContainerNetworkingMode::Bridged);
```
