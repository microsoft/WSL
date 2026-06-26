# Delegates and Events

The WinRT delegates project to normal C# delegates and are consumed as normal C# events.

```csharp
public delegate void SessionTerminationHandler(SessionTerminationReason reason);
public delegate void ProcessCrashHandler(ProcessCrashInformation information);
public delegate void ProcessOutputHandler(byte[] data);
public delegate void ProcessExitHandler(int exitCode);
```

Typical event usage:

```csharp
using System.Text;

session.Terminated += reason => Console.WriteLine($"Session ended: {reason}");
session.ProcessCrashed += info => Console.WriteLine($"Process crashed: {info.ProcessName} ({info.Pid})");
container.InitProcess.OutputReceived += data => Console.Write(Encoding.UTF8.GetString(data));
container.InitProcess.ErrorReceived += data => Console.Error.Write(Encoding.UTF8.GetString(data));
container.InitProcess.Exited += code => Console.WriteLine($"Init exited: {code}");
```

---
