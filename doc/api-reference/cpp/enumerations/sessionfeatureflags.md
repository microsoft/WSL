# SessionFeatureFlags

- `None` is the default in `winrt_SessionSettings.h`.
- `SessionSettings::FeatureFlags` is passed directly to `WslcSetSessionSettingsFeatureFlags`.

Underlying C values:
- `None = 0x00000000`
- `EnableGpu = 0x00000004`

```cpp
SessionSettings settings{ L"demo", L"C:\\WSLC\\demo" };
settings.FeatureFlags(SessionFeatureFlags::None);
```
