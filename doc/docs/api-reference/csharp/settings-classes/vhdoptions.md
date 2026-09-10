# VhdOptions

Describes a session VHD requirement or a named session VHD volume.

```csharp
public sealed class VhdOptions
{
    public VhdOptions(string name, ulong size, VhdType type);

    public string Name { get; set; }
    public ulong Size { get; set; }
    public VhdType Type { get; set; }
    public VhdOwner? Owner { get; set; }
}
```

Notes:

- Use `SessionSettings.VhdRequirements` for session-level storage requirements.
- Use `Session.CreateVhdVolume(...)` for named session volumes.
- `Owner` is intended for named-volume creation and is rejected on `SessionSettings.VhdRequirements`.

Example:

```csharp
var vhd = new VhdOptions("cache", 2UL * 1024 * 1024 * 1024, VhdType.Dynamic)
{
    Owner = new VhdOwner { Uid = 1000, Gid = 1000 }
};
```
