# WslcService

Static entry points over the service-level C API.

**Methods**

- `GetMissingComponents()`
- `GetVersion()`
- `InstallWithDependencies()`
- `InstallWithDependenciesAsync()`

**Behavior notes**

- `GetMissingComponents()` returns a `Component` bitmask.
- `GetVersion()` returns a `ServiceVersion` constructed from `major`, `minor`, and `revision`.
- `InstallWithDependencies()` installs dependencies synchronously.
- `InstallWithDependenciesAsync()` runs on a background thread and reports `InstallProgress`.

```cpp
auto missing = WslcService::GetMissingComponents();
if (missing != static_cast<Component>(0))
{
    auto install = WslcService::InstallWithDependenciesAsync();
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

---
