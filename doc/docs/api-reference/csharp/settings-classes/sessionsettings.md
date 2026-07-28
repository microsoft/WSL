# SessionSettings

Configures a session before `Session.Start()`.

```csharp
public sealed class SessionSettings
{
    public SessionSettings(string name, string storagePath);

    public string Name { get; set; }
    public string StoragePath { get; set; }
    public uint? CpuCount { get; set; }
    public uint? MemorySizeInMB { get; set; }
    public TimeSpan? Timeout { get; set; }
    public VhdOptions VhdRequirements { get; set; }
    public bool EnableGpu { get; set; }
}
```

Notes:

- `Name` is the name of the session to be created.
- `StoragePath` is the path to where the session storage should be written. If the path doesn't exist, it will be created.
- `CpuCount`, `MemorySizeInMB`, and `Timeout` are optional nullable values.
- `Timeout` must be positive and must fit in a `uint32` millisecond count.
- `VhdRequirements` is optional.

Session names serve both as display names and as machine-wide keys used to identify sessions. If a session with the same name already exists, session creation will fail with `ERROR_ALREADY_EXISTS`.

Also note that the following information about a session is visible to all users on the machine:

- The session's name
- The SID of the user that created the session
- The PID of the process that created the session

Do not put credentials or other sensitive information in the session's name.

Example:

```csharp
var sessionSettings = new SessionSettings("demo-session", @"C:\WslcData")
{
    CpuCount = 4,
    MemorySizeInMB = 4096,
    Timeout = TimeSpan.FromMinutes(5),
    EnableGpu = true
};
```
