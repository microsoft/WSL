# WslcService

Static entry points over the service-level C API.

**Methods**

- `GetMissingComponents()`
- `GetVersion()`
- `InstallWithDependencies(InstallOptions options)`
- `InstallWithDependenciesAsync(InstallOptions options)`

**Behavior notes**

- `GetMissingComponents()` returns a view of missing `Component` values.
- `GetVersion()` returns a `ServiceVersion` constructed from `major`, `minor`, and `revision`.
- `InstallWithDependencies()` installs the selected components synchronously.
- `InstallWithDependenciesAsync()` runs on a background thread and reports `InstallProgress`.
- `Component::SdkNeedsUpdate` must be handled separately because the running SDK cannot update itself.

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
    auto install = WslcService::InstallWithDependenciesAsync(options);
    install.Progress([](auto&&, InstallProgress const& p)
    {
        printf("install %u/%u\n", p.Progress(), p.Total());
    });
    co_await install;
}
```

```cpp
auto version = WslcService::GetVersion();
(void)version;
```
