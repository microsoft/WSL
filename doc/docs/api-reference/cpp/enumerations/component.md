# Component

`WslcService::GetMissingComponents()` returns a view of missing components.

Underlying values:

- `VirtualMachinePlatform = 1`
- `WslPackage = 2`
- `SdkNeedsUpdate = 4`

`SdkNeedsUpdate` reports that the client SDK package must be updated. It cannot be installed by
`WslcService`; passing it to `InstallWithDependencies` raises an error.

```cpp
auto missing = WslcService::GetMissingComponents();
std::vector<Component> installable;
for (auto component : missing)
{
    if (component == Component::SdkNeedsUpdate)
    {
        printf("Update the Microsoft.WSL.Containers SDK package.\n");
    }
    else
    {
        installable.push_back(component);
    }
}

if (!installable.empty())
{
    auto components = winrt::single_threaded_vector<Component>(std::move(installable));
    InstallOptions options;
    options.Components(components.GetView());
    co_await WslcService::InstallWithDependenciesAsync(options);
}
```
