# ImageInfo

Read-only wrapper created from `WslcImageInfo`.

**Properties**

- `Name()` → image name
- `Sha256()` → 32-byte buffer
- `Size()` → image size
- `CreatedTimestamp()` → WinRT `DateTime`

```cpp
auto images = session.GetImages();
for (auto const& image : images)
{
    auto name = image.Name();
    auto hash = image.Sha256();
    auto size = image.Size();
    auto created = image.CreatedTimestamp();
}
```
