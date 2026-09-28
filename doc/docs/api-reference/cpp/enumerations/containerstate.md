# ContainerState

`Container::State()` casts directly from `WslcContainerState`.

Underlying C values:

- `Invalid = 0`
- `Created = 1`
- `Running = 2`
- `Exited = 3`
- `Deleted = 4`

```cpp
auto state = container.State();
if (state == static_cast<ContainerState>(2))
{
    // running
}
```
