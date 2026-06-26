# ProcessSettings

**Properties**
- `WorkingDirectory()` / setter
- `CmdLine()` / setter
- `EnvironmentVariables()` / setter
- `OutputMode()` / setter

**Important notes**
- `CmdLine(nullptr)` and `EnvironmentVariables(nullptr)` are rejected.
- `Process::Start()` later requires a **non-empty** `CmdLine()`.
- `OutputMode::Event` installs C callbacks; `OutputMode::Stream` expects stream access; `Discard` is the default.

```cpp
using namespace winrt::Windows::Foundation::Collections;

ProcessSettings procSettings;
procSettings.WorkingDirectory(L"/workspace");
procSettings.OutputMode(ProcessOutputMode::Event);

auto cmd = single_threaded_vector<hstring>();
cmd.Append(L"/bin/sh");
cmd.Append(L"-lc");
cmd.Append(L"echo hello");
procSettings.CmdLine(cmd);

auto env = single_threaded_map<hstring, hstring>();
env.Insert(L"DEMO", L"1");
procSettings.EnvironmentVariables(env);
```

---
