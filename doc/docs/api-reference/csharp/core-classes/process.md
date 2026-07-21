# Process

Represents a Linux process in a container.

```csharp
using Windows.Storage.Streams;

public sealed class Process
{
    public uint Pid { get; }
    public ProcessState State { get; }
    public int ExitCode { get; }

    public event ProcessOutputHandler OutputReceived;
    public event ProcessOutputHandler ErrorReceived;
    public event ProcessExitHandler Exited;

    public void Start();
    public void Signal(Signal signal);
    public IInputStream GetOutputStream(ProcessOutputHandle outputHandle);
    public IOutputStream GetInputStream();
}
```

Notes:

- Call `Start()` only for **secondary processes** created by `Container.CreateProcess(...)`.
- The init process is started by `Container.Start()`.
- `OutputReceived` and `ErrorReceived` require `OutputMode.Event`.
- `GetOutputStream(...)` requires `OutputMode.Stream`.
- `Exited` is available for all output modes.

## Process.Start()

Starts a secondary process.

```csharp
process.Start();
```

## Process.Signal(Signal)

Signals the process.

```csharp
process.Signal(Signal.SIGTERM);
```

## Process.GetOutputStream(ProcessOutputHandle)

Gets stdout or stderr as a WinRT input stream.

```csharp
using Windows.Storage.Streams;

using IInputStream stdout = process.GetOutputStream(ProcessOutputHandle.StandardOutput);
using var reader = new DataReader(stdout);
await reader.LoadAsync(4096);
string text = reader.ReadString(reader.UnconsumedBufferLength);
Console.WriteLine(text);
```

## Process.GetInputStream()

Gets stdin as a WinRT output stream.

```csharp
using Windows.Storage.Streams;

using IOutputStream stdin = process.GetInputStream();
using var writer = new DataWriter(stdin);
writer.WriteString("hello from C#\n");
await writer.StoreAsync();
await writer.FlushAsync();
```

## Process.Pid

```csharp
Console.WriteLine($"PID: {process.Pid}");
```

## Process.State

```csharp
Console.WriteLine($"State: {process.State}");
```

## Process.ExitCode

Valid after exit.

```csharp
Console.WriteLine($"Exit code: {process.ExitCode}");
```

## Process.OutputReceived event

```csharp
using System.Text;

process.OutputReceived += data =>
    Console.Write(Encoding.UTF8.GetString(data));
```

## Process.ErrorReceived event

```csharp
using System.Text;

process.ErrorReceived += data =>
    Console.Error.Write(Encoding.UTF8.GetString(data));
```

## Process.Exited event

```csharp
process.Exited += exitCode =>
    Console.WriteLine($"Process exited with {exitCode}");
```

---
