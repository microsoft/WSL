# ServiceVersion

Version returned by `WslcService::GetVersion()`.

**Properties**

- `Major()`
- `Minor()`
- `Revision()`

```cpp
auto version = WslcService::GetVersion();
printf("%u.%u.%u\n", version.Major(), version.Minor(), version.Revision());
```
