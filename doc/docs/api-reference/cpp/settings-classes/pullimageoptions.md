# PullImageOptions

**Constructor**

- `PullImageOptions(hstring uri)`

**Properties**

| Property | Type |
|---|---|
| `Uri()` / setter | `hstring` |
| `RegistryAuth()` / setter | `hstring` |

```cpp
PullImageOptions options{L"docker.io/library/alpine:latest"};
options.RegistryAuth(L"");
```
