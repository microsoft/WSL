# VhdType

Underlying values:

- `Dynamic = 0`
- `Fixed = 1`
- `Sparse = 2`

```cpp
vhdOptions.Type(VhdType::Dynamic);
```

`Sparse` creates a dynamically expanding VHD backed by a sparse host file. This feature is currently experimental.
