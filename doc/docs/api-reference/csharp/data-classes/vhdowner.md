# VhdOwner

Owner uid/gid for a named VHD volume root inode.

```csharp
public struct VhdOwner
{
    public uint Uid;
    public uint Gid;
}
```

Example:

```csharp
var owner = new VhdOwner { Uid = 1000, Gid = 1000 };
```
