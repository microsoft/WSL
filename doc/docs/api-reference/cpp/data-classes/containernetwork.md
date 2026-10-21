# ContainerNetwork

Connects a container to a network.

**Constructors / properties**

- `ContainerNetwork(hstring name)`
  - `name` must be non-empty.
- `Name()` / setter
- `Aliases()` / setter
  - Only supported on user-defined networks (not `bridge`, `host`, `none`, or `container:<name>`).

```cpp
ContainerNetwork network{ L"frontend" };
network.Aliases().Append(L"web");
```
