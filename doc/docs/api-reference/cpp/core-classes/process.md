# Process

`Process` objects are returned by `Container::CreateProcess()` and `Container::InitProcess()`.

**Methods / events**

- `Start()`
- `Signal(Signal signal)`
- `GetOutputStream(ProcessOutputHandle outputHandle)`
- `GetInputStream()`
- `Pid()`
- `State()`
- `ExitCode()`
- event `OutputReceived`
- event `ErrorReceived`
- event `Exited`
- `Close()`

**Behavior notes**

- `Start()` cannot be called on the init process.
- `Start()` requires a non-empty `ProcessSettings::CommandLine()`.
- `GetOutputStream()` requires `ProcessOutputMode::Stream`.
- `OutputReceived` and `ErrorReceived` require `ProcessOutputMode::Event`.
- `Exited` is raised by the exit callback in event mode, and by waiting on the process exit event in stream/discard mode.

**Examples**

```cpp
auto proc = container.CreateProcess(procSettings);
proc.Exited([](int32_t exitCode)
{
    printf("process exited: %d\n", exitCode);
});
proc.Start();
```

```cpp
auto pid = proc.Pid();
auto state = proc.State();
```

```cpp
proc.Signal(static_cast<Signal>(2)); // SIGINT
```

```cpp
auto stdinStream = proc.GetInputStream();
```

```cpp
ProcessSettings streamSettings;
streamSettings.OutputMode(ProcessOutputMode::Stream);
// ... set CommandLine ...
auto streamProc = container.CreateProcess(streamSettings);
streamProc.Start();
auto stdoutStream = streamProc.GetOutputStream(static_cast<ProcessOutputHandle>(1));
auto stderrStream = streamProc.GetOutputStream(static_cast<ProcessOutputHandle>(2));
```

```cpp
ProcessSettings eventSettings;
eventSettings.OutputMode(ProcessOutputMode::Event);
// ... set CommandLine ...
auto eventProc = container.CreateProcess(eventSettings);
eventProc.OutputReceived([](auto const& data)
{
    printf("stdout bytes: %zu\n", data.size());
});
eventProc.ErrorReceived([](auto const& data)
{
    printf("stderr bytes: %zu\n", data.size());
});
eventProc.Exited([](int32_t exitCode)
{
    printf("done: %d\n", exitCode);
});
eventProc.Start();
```

```cpp
auto exitCode = proc.ExitCode();
```

---
