# PullImageOptions

**Constructor**

- `PullImageOptions(hstring uri)`

**Properties**

- `Uri()` / setter
- `RegistryAuth()` / setter

```cpp
PullImageOptions options{L"docker.io/library/alpine:latest"};
options.RegistryAuth(L"");
```
