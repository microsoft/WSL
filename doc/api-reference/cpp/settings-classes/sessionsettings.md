# SessionSettings

**Constructor**
- `SessionSettings(hstring name, hstring storagePath)`
  - `name` must be non-empty.
  - `storagePath` must be non-empty.

**Properties**
- `Name()` / setter
- `StoragePath()` / setter
- `CpuCount()` / setter (`0` rejected)
- `MemoryMB()` / setter (`0` rejected)
- `Timeout()` / setter
  - cannot be zero
  - cannot be negative
  - converted to **milliseconds** and must fit in `uint32_t`
- `VhdRequirements()` / setter
  - setter rejects `nullptr`
- `FeatureFlags()` / setter

```cpp
SessionSettings settings{ L"demo", L"C:\\WSLC\\demo" };
settings.Name(L"demo");
settings.StoragePath(L"C:\\WSLC\\demo");
settings.CpuCount(winrt::box_value<uint32_t>(4).as<winrt::Windows::Foundation::IReference<uint32_t>>());
settings.MemoryMB(winrt::box_value<uint32_t>(4096).as<winrt::Windows::Foundation::IReference<uint32_t>>());
settings.Timeout(winrt::box_value(winrt::Windows::Foundation::TimeSpan{ std::chrono::minutes(5) })
    .as<winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::TimeSpan>>());
settings.FeatureFlags(SessionFeatureFlags::None);

auto name = settings.Name();
auto path = settings.StoragePath();
auto cpu = settings.CpuCount();
auto memory = settings.MemoryMB();
auto timeout = settings.Timeout();
auto flags = settings.FeatureFlags();
```
