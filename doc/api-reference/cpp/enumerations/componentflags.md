# ComponentFlags

`WslcService` converts directly to/from `WslcComponentFlags`.

Underlying C values:
- `0` none
- `1` virtual machine platform
- `2` WSL package
- `4` SDK needs update

\

```cpp
auto missing = WslcService::GetMissingComponents();
if (missing != static_cast<ComponentFlags>(0))
{
    co_await WslcService::InstallWithDependenciesAsync();
}
```

---
