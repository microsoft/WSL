# DeleteContainerFlags

`Container::Delete()` casts directly to `WslcDeleteContainerFlags`.

Underlying C values:
- `0` none
- `0x01` force

```cpp
container.Delete(static_cast<DeleteContainerFlags>(0x01));
```
