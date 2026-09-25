# ServiceVersion

Version returned by `WslcService::GetVersion()`.

**Properties**

| Property | Type |
|---|---|
| `Major()` | `uint32_t` |
| `Minor()` | `uint32_t` |
| `Revision()` | `uint32_t` |

```cpp
auto version = WslcService::GetVersion();
printf("%u.%u.%u\n", version.Major(), version.Minor(), version.Revision());
```
