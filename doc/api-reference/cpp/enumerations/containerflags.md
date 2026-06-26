# ContainerFlags

- `None` is the default in `winrt_ContainerSettings.h`.
- Flags are passed directly to `WslcSetContainerSettingsFlags`.

Underlying C values:
- `None = 0x00000000`
- `AutoRemove = 0x00000001`
- `EnableGpu = 0x00000002`
- `Privileged = 0x00000004`

```cpp
containerSettings.Flags(static_cast<ContainerFlags>(0x00000001)); // AutoRemove
```
