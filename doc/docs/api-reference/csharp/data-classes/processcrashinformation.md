# ProcessCrashInformation

Crash information supplied by the `Session.ProcessCrashed` event.

```csharp
public sealed class ProcessCrashInformation
{
    public string DumpPath { get; }
    public string ProcessName { get; }
    public uint Pid { get; }
    public uint Signal { get; }
    public DateTimeOffset Timestamp { get; }
}
```

Example:

```csharp
session.ProcessCrashed += information =>
    Console.WriteLine($"{information.ProcessName} ({information.Pid}) crashed at {information.Timestamp}");
```
