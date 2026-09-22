# InstallProgress

Progress item reported by `WslcService::InstallWithDependenciesAsync(InstallOptions)`.

**Properties**

- `Component()`
- `Progress()`
- `Total()`

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
        printf("component=%d step=%u/%u\n",
            static_cast<int>(p.Component()),
            p.Progress(),
            p.Total());
    });
    co_await install;
}
```
