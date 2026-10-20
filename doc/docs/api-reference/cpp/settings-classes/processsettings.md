# ProcessSettings

**Properties**

- `WorkingDirectory()` / setter
- `CommandLine()` / setter
- `EnvironmentVariables()` / setter
- `OutputMode()` / setter
- `EnableStandardInput()` / setter

**Important notes**

- `CommandLine(nullptr)` and `EnvironmentVariables(nullptr)` are rejected.
- `Process::Start()` later requires a **non-empty** `CommandLine()`.
- `ProcessOutputMode::Event` installs C callbacks; `ProcessOutputMode::Stream` expects stream access; `Discard` is the default.
- `EnableStandardInput()` defaults to `false`. Must be set before `Process::Start()` or `Session::CreateContainer()` to have effect.

```cpp
using namespace winrt::Windows::Foundation::Collections;

ProcessSettings procSettings;
procSettings.WorkingDirectory(L"/workspace");
procSettings.OutputMode(ProcessOutputMode::Event);

auto cmd = single_threaded_vector<hstring>();
cmd.Append(L"/bin/sh");
cmd.Append(L"-lc");
cmd.Append(L"echo hello");
procSettings.CommandLine(cmd);

auto env = single_threaded_map<hstring, hstring>();
env.Insert(L"DEMO", L"1");
procSettings.EnvironmentVariables(env);
```

---
