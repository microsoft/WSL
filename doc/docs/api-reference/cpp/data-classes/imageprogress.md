# ImageProgress

Progress item reported by image pull/import/load/push operations.

**Properties**

- `Id()`
- `Status()`
- `CurrentBytes()`
- `TotalBytes()`

```cpp
auto op = session.LoadImageAsync(L"C:\\images\\demo.tar");
op.Progress([](auto&&, ImageProgress const& p)
{
    printf("layer=%ws status=%d %llu/%llu\n",
        p.Id().c_str(),
        static_cast<int>(p.Status()),
        p.CurrentBytes(),
        p.TotalBytes());
});
co_await op;
```
