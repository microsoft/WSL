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
- If `GetMissingComponents()` returns `Component::SdkNeedsUpdate`, installation cannot resolve the
  SDK compatibility error.

```cpp
auto missing = WslcService::GetMissingComponents();
for (auto component : missing)
{
    if (component == Component::SdkNeedsUpdate)
    {
        // Installing components cannot resolve this compatibility error.
        co_return;
    }
}

if (missing.Size() != 0)
{
    InstallOptions options;
    options.Components(missing);
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
