# TagImageOptions

**Constructor**

- `TagImageOptions(hstring image, hstring repository, hstring tag)`

**Properties**

- `Image()` / setter
- `Repository()` / setter
- `Tag()` / setter

```cpp
TagImageOptions options{L"alpine:latest", L"registry.example.com/alpine", L"v1"};
session.TagImage(options);
```
