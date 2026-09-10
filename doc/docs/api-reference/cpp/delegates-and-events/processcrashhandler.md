# ProcessCrashHandler

Observed use:

- `Session::ProcessCrashed` raises one `ProcessCrashInformation` argument.

```cpp
session.ProcessCrashed([](ProcessCrashInformation const& info)
{
    printf("process crashed: %ws\n", info.ProcessName().c_str());
});
```
