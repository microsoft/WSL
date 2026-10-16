# TagImageOptions

**Constructor**

- `TagImageOptions(hstring image, hstring repository, hstring tag)`

**Properties**

| Property | Type |
|---|---|
| `Image()` / setter | `hstring` |
| `Repository()` / setter | `hstring` |
| `Tag()` / setter | `hstring` |

```cpp
TagImageOptions options{L"alpine:latest", L"registry.example.com/alpine", L"v1"};
session.TagImage(options);
```
