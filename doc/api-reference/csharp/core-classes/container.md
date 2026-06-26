# Container

Represents a container inside a session.

```csharp
public sealed class Container
{
    public string Id { get; }
    public Process InitProcess { get; }
    public ContainerState State { get; }

    public void Start();
    public void Stop(Signal signal, TimeSpan timeout);
    public void Delete(DeleteContainerFlags flags);
    public Process CreateProcess(ProcessSettings newProcessSettings);
    public string Inspect();
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

## Container.Delete(DeleteContainerFlags)

Deletes the container.

```csharp
container.Delete(DeleteContainerFlags.Force);
```

## Container.CreateProcess(ProcessSettings)

Creates a secondary process object inside the container.

```csharp
var execSettings = new ProcessSettings
{
    CmdLine = new List<string> { "/bin/sh", "-c", "echo secondary process" },
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
