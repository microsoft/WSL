# ProcessCrashInformation

Crash information supplied by the `Session::ProcessCrashed` event.

**Properties**

| Property | Type |
|---|---|
| `DumpPath()` | `hstring` |
| `ProcessName()` | `hstring` |
| `Pid()` | `uint32_t` |
| `Signal()` | `uint32_t` |
| `Timestamp()` | `winrt::Windows::Foundation::DateTime` |

```cpp
session.ProcessCrashed([](ProcessCrashInformation const& information)
{
    printf("%ws crashed with signal %u\n", information.ProcessName().c_str(), information.Signal());
});
```
