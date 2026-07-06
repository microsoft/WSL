# Component

`WslcService::GetMissingComponents()` returns a `Component` bitmask.

Underlying values:

- `VirtualMachinePlatform = 1`
- `WslPackage = 2`
- `SdkNeedsUpdate = 4`

```cpp
auto missing = WslcService::GetMissingComponents();
if (missing != static_cast<Component>(0))
{
    co_await WslcService::InstallWithDependenciesAsync();
}
```
