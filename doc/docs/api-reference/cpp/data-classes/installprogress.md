# InstallProgress

Progress item reported by `WslcService::InstallWithDependenciesAsync(InstallOptions)`.

**Properties**

| Property | Type |
|---|---|
| `Component()` | `Component` |
| `Progress()` | `uint32_t` |
| `Total()` | `uint32_t` |

```cpp
auto missing = WslcService::GetMissingComponents();
for (auto component : missing)
{
    if (component == Component::SdkNeedsUpdate)
    {
        printf("Update this application to a version that uses a compatible Microsoft.WSL.Containers SDK.\n");
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
        printf("component=%d step=%u/%u\n",
            static_cast<int>(p.Component()),
            p.Progress(),
            p.Total());
    });
    co_await install;
}
```
