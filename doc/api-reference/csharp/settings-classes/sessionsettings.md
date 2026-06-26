# SessionSettings

Configures a session before `Session.Start()`.

```csharp
public sealed class SessionSettings
{
    public SessionSettings(string name, string storagePath);

    public string Name { get; set; }
    public string StoragePath { get; set; }
    public uint? CpuCount { get; set; }
    public uint? MemoryMB { get; set; }
    public TimeSpan? Timeout { get; set; }
    public VhdOptions VhdRequirements { get; set; }
    public SessionFeatureFlags FeatureFlags { get; set; }
}
```

Notes:
- `CpuCount`, `MemoryMB`, and `Timeout` are optional nullable values.
- `Timeout` must be positive and must fit in a `uint32` millisecond count.
- `VhdRequirements` is optional.

Example:

```csharp
var sessionSettings = new SessionSettings("demo-session", @"C:\WslcData")
{
    CpuCount = 4,
    MemoryMB = 4096,
    Timeout = TimeSpan.FromMinutes(5),
    FeatureFlags = SessionFeatureFlags.EnableGpu
};
```
