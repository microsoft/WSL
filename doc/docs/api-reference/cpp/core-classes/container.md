# Container

**Methods**

- `Start()`
- `Stop(Signal signal, TimeSpan timeout)`
- `Delete(DeleteContainerOption options)`
- `CreateProcess(ProcessSettings newProcessSettings)`
- `Inspect()`
- `Id()`
- `InitProcess()`
- `State()`
- `Close()`

**Behavior notes**

- `Start()` automatically sets `WSLC_CONTAINER_START_FLAG_ATTACH` if an init process exists and its output mode is `Event` or `Stream`.
- `Stop()` converts the timeout to **seconds**, rejects negative values, and rejects values that exceed `uint32_t`.
- `InitProcess()` throws if the container was not configured with one.

**Examples**

```cpp
container.Start();
```

```cpp
container.Stop(static_cast<Signal>(15), std::chrono::seconds(10));
```

```cpp
container.Delete(DeleteContainerOption::None);
```

```cpp
auto proc = container.CreateProcess(procSettings);
```

```cpp
auto inspectJson = container.Inspect();
auto id = container.Id();
auto state = container.State();
```

```cpp
auto init = container.InitProcess();
```
