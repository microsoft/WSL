# VhdOptions

Describes a session VHD requirement or a named session VHD volume.

```csharp
public sealed class VhdOptions
{
    public VhdOptions(string name, ulong sizeInBytes, VhdType type);

    public string Name { get; set; }
    public ulong SizeInBytes { get; set; }
    public VhdType Type { get; set; }

    public void SetOwner(uint uid, uint gid);
}
```

Notes:
- Use `SessionSettings.VhdRequirements` for session-level storage requirements.
- Use `Session.CreateVhdVolume(...)` for named session volumes.
- Owner flags are intended for named-volume creation. The C header explicitly says owner flags are rejected by `WslcSetSessionSettingsVhd`.

Example:

```csharp
var vhd = new VhdOptions("cache", 2UL * 1024 * 1024 * 1024, VhdType.Dynamic);
vhd.SetOwner(1000, 1000);
```
