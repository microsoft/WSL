# Container

**Methods**

- `Start()`
- `Stop(Signal signal, TimeSpan timeout)`
- `Delete(DeleteContainerOption options)`
- `ConnectNetwork(ContainerNetwork network)`
- `DisconnectNetwork(hstring nameOrId)`
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
- `ConnectNetwork()` and `DisconnectNetwork()` work on created or running containers. The network may be referenced by name, full ID, or unique ID prefix. `ConnectNetwork()` throws if `network` is null; `DisconnectNetwork()` throws if `nameOrId` is empty.
- `ConnectNetwork()` and `DisconnectNetwork()` require WSL 3.1.0 or later and throw `WSLC_E_RUNTIME_UPDATE_NEEDED` on older runtimes.

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
ContainerNetwork network{ L"backend" };
network.Aliases().Append(L"web");
container.ConnectNetwork(network);
container.DisconnectNetwork(L"backend");
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
