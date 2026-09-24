# ProcessSettings

Configures a process before start.

```csharp
public sealed class ProcessSettings
{
    public string WorkingDirectory { get; set; }
    public IList<string> CommandLine { get; set; }
    public IDictionary<string, string> EnvironmentVariables { get; set; }
    public ProcessOutputMode OutputMode { get; set; }
    public bool EnableStandardInput { get; set; }
}
```

Notes:

- `CommandLine` must be non-empty before calling `Process.Start()`.
- The init process is started by `Container.Start()`, not by `Process.Start()`.
- `OutputMode.Event` enables `OutputReceived` / `ErrorReceived`.
- `OutputMode.Stream` enables `GetOutputStream(...)`.
- `EnableStandardInput` defaults to `false`. Set it to `true` before the settings are applied to use `Process.GetInputStream()`.

Example:

```csharp
var processSettings = new ProcessSettings
{
    WorkingDirectory = "/workspace",
    CommandLine = new List<string> { "/bin/sh", "-c", "env | sort" },
    EnvironmentVariables = new Dictionary<string, string>
    {
        ["DEMO"] = "1",
        ["PATH"] = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
    },
    OutputMode = ProcessOutputMode.Event
};
```

---
