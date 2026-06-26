# WslcService

Static entry points over the service-level C API.

**Methods**
- `GetMissingComponents()`
- `GetVersion()`
- `InstallWithDependenciesAsync()`

**Behavior notes**
- `GetMissingComponents()` returns the direct cast of `WslcComponentFlags`.
- `GetVersion()` returns a `ServiceVersion` constructed from `major`, `minor`, and `revision`.
- `InstallWithDependenciesAsync()` runs on a background thread and reports `InstallProgress`.

```cpp
auto missing = WslcService::GetMissingComponents();
if (missing != static_cast<ComponentFlags>(0))
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
