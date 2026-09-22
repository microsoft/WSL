# PushImageOptions

**Constructor**

- `PushImageOptions(hstring image, hstring registryAuth)`

**Properties**

- `Image()` / setter
- `RegistryAuth()` / setter

```cpp
PushImageOptions options{L"registry.example.com/demo:latest", authentication.IdentityToken()};
```
