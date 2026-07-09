# InstallProgress

Progress item reported by `WslcService::InstallWithDependenciesAsync()`.

**Properties**

- `Component()`
- `Progress()`
- `Total()`

```cpp
auto install = WslcService::InstallWithDependenciesAsync();
install.Progress([](auto&&, InstallProgress const& p)
{
    printf("component=%d step=%u/%u\n",
        static_cast<int>(p.Component()),
        p.Progress(),
        p.Total());
});
co_await install;
```
