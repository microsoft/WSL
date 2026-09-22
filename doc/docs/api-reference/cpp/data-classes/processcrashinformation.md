# ProcessCrashInformation

Crash information supplied by the `Session::ProcessCrashed` event.

**Properties**

- `DumpPath()`
- `ProcessName()`
- `Pid()`
- `Signal()`
- `Timestamp()`

```cpp
session.ProcessCrashed([](ProcessCrashInformation const& information)
{
    printf("%ws crashed with signal %u\n", information.ProcessName().c_str(), information.Signal());
});
```
