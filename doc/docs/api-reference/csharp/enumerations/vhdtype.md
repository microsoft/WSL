# VhdType

```csharp
public enum VhdType
{
    Dynamic = 0,
    Fixed = 1
}
```

> `wslcsdk.h` notes that `Fixed` is only honored for `WslcCreateSessionVhdVolume`. Session boot-disk requirements use the same underlying struct, but owner flags are explicitly rejected there.

---
