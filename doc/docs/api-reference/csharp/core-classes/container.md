# Container

Represents a container inside a session.

```csharp
public sealed class Container : IDisposable
{
    public string Id { get; }
    public Process InitProcess { get; }
    public ContainerState State { get; }

    public void Start();
    public void Stop(Signal signal, TimeSpan timeout);
    public void Delete(DeleteContainerOption option);
    public void ConnectNetwork(ContainerNetwork network);
    public void DisconnectNetwork(string nameOrId);
    public Process CreateProcess(ProcessSettings newProcessSettings);
    public string Inspect();
    public void Dispose();
}
```

Notes:

- `Start()` has **no flags parameter**.
- If `InitProcess.OutputMode` is `Event` or `Stream`, `Start()` automatically requests native attach.
- `InitProcess` is only available when `ContainerSettings.InitProcess` was configured.

## Container.Start()

Starts the container and, if configured, attaches the init process handle.

```csharp
container.Start();
```

## Container.Stop(Signal, TimeSpan)

Stops the container with a signal and timeout.

```csharp
container.Stop(Signal.SIGTERM, TimeSpan.FromSeconds(10));
```

## Container.Delete(DeleteContainerOption)

Deletes the container.

```csharp
container.Delete(DeleteContainerOption.Force);
```

## Container.ConnectNetwork(ContainerNetwork)

Connects a created or running container to a session network. The network may be referenced by name, full ID, or unique ID prefix. Throws if `network` is `null`.

Requires WSL 3.1.0 or later; throws with `RuntimeUpdateNeeded` on older runtimes.

```csharp
container.ConnectNetwork(new ContainerNetwork("backend")
{
    Aliases = new List<string> { "web" }
});
```

## Container.DisconnectNetwork(string)

Disconnects a created or running container from a session network. `nameOrId` is the name, full ID, or unique ID prefix of the network. Throws if `nameOrId` is empty.

Requires WSL 3.1.0 or later; throws with `RuntimeUpdateNeeded` on older runtimes.

```csharp
container.DisconnectNetwork("backend");
```

## Container.CreateProcess(ProcessSettings)

Creates a secondary process object inside the container.

```csharp
var execSettings = new ProcessSettings
{
    CommandLine = new List<string> { "/bin/sh", "-c", "echo secondary process" },
    OutputMode = ProcessOutputMode.Event
};

Process process = container.CreateProcess(execSettings);
```

## Container.Inspect()

Returns the raw inspect payload as a string.

```csharp
string inspectJson = container.Inspect();
Console.WriteLine(inspectJson);
```

## Container.Id

Returns the container ID string.

```csharp
Console.WriteLine(container.Id);
```

## Container.InitProcess

Gets the configured init process object.

```csharp
Process init = container.InitProcess;
```

## Container.State

Gets the current container state.

```csharp
Console.WriteLine(container.State);
```

## Container.Dispose()

Releases the underlying WinRT container object.

```csharp
container.Dispose();
```
