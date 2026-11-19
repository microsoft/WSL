# VhdType

```csharp
public enum VhdType
{
    Dynamic = 0,
    Fixed = 1,
    Sparse = 2
}
```

`Sparse` creates a dynamically expanding VHD backed by a sparse host file. This feature is currently experimental and requires WSL 3.1.0
or later.

> `Fixed` is only supported for named volumes. Session storage supports `Dynamic` and `Sparse`.

---
