# SessionSettings

**Constructor**

- `SessionSettings(hstring name, hstring storagePath)`
  - `name` must be non-empty. The name of the session to be created.
  - `storagePath` must be non-empty. Path to where the session storage should be written. If the path doesn't exist, it will be created.

Session names serve both as display names and as machine-wide keys used to identify sessions. If a session with the same name already exists, session creation will fail with `ERROR_ALREADY_EXISTS`.

Also note that the following information about a session is visible to all users on the machine:

- The session's name
- The SID of the user that created the session
- The PID of the process that created the session

Do not put credentials or other sensitive information in the session's name.

**Properties**

- `Name()` / setter
- `StoragePath()` / setter
- `CpuCount()` / setter (`0` rejected)
- `MemorySizeInMB()` / setter (`0` rejected)
- `Timeout()` / setter
  - cannot be zero
  - cannot be negative
  - converted to **milliseconds** and must fit in `uint32_t`
- `VhdRequirements()` / setter
  - setter rejects `nullptr`
- `EnableGpu()` / setter

```cpp
SessionSettings settings{ L"demo", L"C:\\WSLC\\demo" };
settings.Name(L"demo");
settings.StoragePath(L"C:\\WSLC\\demo");
settings.CpuCount(winrt::box_value<uint32_t>(4).as<winrt::Windows::Foundation::IReference<uint32_t>>());
settings.MemorySizeInMB(winrt::box_value<uint32_t>(4096).as<winrt::Windows::Foundation::IReference<uint32_t>>());
settings.Timeout(winrt::box_value(winrt::Windows::Foundation::TimeSpan{ std::chrono::minutes(5) })
    .as<winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::TimeSpan>>());
settings.EnableGpu(true);

auto name = settings.Name();
auto path = settings.StoragePath();
auto cpu = settings.CpuCount();
auto memory = settings.MemorySizeInMB();
auto timeout = settings.Timeout();
auto enableGpu = settings.EnableGpu();
```
