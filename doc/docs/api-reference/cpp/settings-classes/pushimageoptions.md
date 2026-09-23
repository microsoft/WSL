# PushImageOptions

**Constructor**

- `PushImageOptions(hstring image, hstring registryAuth)`

**Properties**

| Property | Type |
|---|---|
| `Image()` / setter | `hstring` |
| `RegistryAuth()` / setter | `hstring` |

```cpp
PushImageOptions options{L"registry.example.com/demo:latest", authentication.IdentityToken()};
```
