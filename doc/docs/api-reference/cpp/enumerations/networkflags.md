# NetworkFlags

`NetworkInfo::Flags()` returns `NetworkFlags`.

Underlying values (match `WslcNetworkFlags`):

- `None = 0`
- `EnableIPv4 = 1`
- `EnableIPv6 = 2`
- `Internal = 4`

```cpp
bool isInternal = (network.Flags() & NetworkFlags::Internal) == NetworkFlags::Internal;
```