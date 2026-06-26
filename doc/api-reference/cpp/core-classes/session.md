# Session

**Constructor**
- `Session(SessionSettings settings)`
  - rejects `nullptr` settings.

**Methods**
- `Start()`
- `Terminate()`
- `CreateContainer(ContainerSettings containerSettings)`
- `PullImageAsync(PullImageOptions options)`
- `ImportImageAsync(hstring path, hstring imageName)`
- `LoadImageAsync(hstring path)`
- `PushImageAsync(PushImageOptions options)`
- `DeleteImage(hstring nameOrId)`
- `TagImage(TagImageOptions options)`
- `CreateVhdVolume(VhdOptions options)`
- `DeleteVhdVolume(hstring name)`
- `Authenticate(Uri serverAddress, hstring username, hstring password)`
- `Images()`
- event `Terminated`

**Behavior notes**
- `Start()` is one-shot; calling it twice throws.
- Most methods call `EnsureStarted()` first.
- `ImportImageAsync` and `LoadImageAsync` are path-based only.
- `Authenticate` requires a non-null `Uri` and non-empty username.
- `Images()` materializes WinRT `ImageInfo` objects from the C array returned by `WslcListSessionImages`.

**Examples**

```cpp
Session session{ settings };
session.Terminated([](SessionTerminationReason reason)
{
    printf("session terminated: %d\n", static_cast<int>(reason));
});
session.Start();
```

```cpp
auto container = session.CreateContainer(containerSettings);
```

```cpp
auto importOp = session.ImportImageAsync(L"C:\\images\\alpine.tar", L"demo/alpine:latest");
importOp.Progress([](auto&&, ImageProgress const& p) { /* progress */ });
co_await importOp;
```

```cpp
auto loadOp = session.LoadImageAsync(L"C:\\images\\bundle.tar");
co_await loadOp;
```

```cpp
session.DeleteImage(L"demo/alpine:latest");
```

```cpp
auto token = session.Authenticate(
    winrt::Windows::Foundation::Uri{ L"https://registry.example.com" },
    L"user",
    L"password");
```

```cpp
auto images = session.Images();
for (auto const& image : images)
{
    printf("%ws\n", image.Name().c_str());
}
```

```cpp
PullImageOptions pullOptions = ;
auto pullOp = session.PullImageAsync(pullOptions);
pullOp.Progress([](auto&&, ImageProgress const& p) { /* progress */ });
co_await pullOp;

PushImageOptions pushOptions = ;
co_await session.PushImageAsync(pushOptions);

TagImageOptions tagOptions = ;
session.TagImage(tagOptions);

VhdOptions vhdOptions = ;
session.CreateVhdVolume(vhdOptions);
session.DeleteVhdVolume(L"build-cache");
```

```cpp
session.Terminate();
```
