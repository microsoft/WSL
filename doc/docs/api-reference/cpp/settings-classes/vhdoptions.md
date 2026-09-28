# VhdOptions

**Properties**

- `Name()` / setter
- `Size()` / setter
- `Type()` / setter
- `Owner()` / setter

```cpp
VhdOptions options;
options.Name(L"build-cache");
options.Size(10ull * 1024 * 1024 * 1024);
options.Type(VhdType::Dynamic);
options.Owner({ 1000, 1000 });
```
