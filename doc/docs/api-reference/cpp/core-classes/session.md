# Session

**Constructor**

- `Session(SessionSettings settings)`
  - rejects `nullptr` settings.

**Methods**

- `Start()`
- `Terminate()`
- `CreateContainer(ContainerSettings containerSettings)`
- `PullImage(PullImageOptions options)`
- `PullImageAsync(PullImageOptions options)`
- `ImportImage(hstring path, hstring imageName)`
- `ImportImageAsync(hstring path, hstring imageName)`
- `LoadImage(hstring path)`
- `LoadImageAsync(hstring path)`
- `PushImage(PushImageOptions options)`
- `PushImageAsync(PushImageOptions options)`
- `DeleteImage(hstring nameOrId)`
- `TagImage(TagImageOptions options)`
- `CreateVhdVolume(VhdOptions options)`
- `DeleteVhdVolume(hstring name)`
- `Authenticate(Uri serverAddress, hstring username, hstring password)`
- `GetImages()`
- event `Terminated`
- event `ProcessCrashed`
- `Close()`

**Behavior notes**

- `Start()` is one-shot; calling it twice throws.
- Most methods call `EnsureStarted()` first.
- `ImportImage` / `ImportImageAsync` and `LoadImage` / `LoadImageAsync` are path-based only.
- `Authenticate` requires a non-null `Uri` and non-empty username.
- `GetImages()` materializes WinRT `ImageInfo` objects from the C array returned by `WslcListSessionImages`.

**Examples**

```cpp
Session session{ settings };
session.Terminated([](SessionTerminationReason reason)
{
    printf("session terminated: %d\n", static_cast<int>(reason));
});
session.ProcessCrashed([](ProcessCrashInformation const& info)
{
    printf("process crashed: %ws\n", info.ProcessName().c_str());
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
auto images = session.GetImages();
for (auto const& image : images)
{
    printf("%ws\n", image.Name().c_str());
}
```

```cpp
PullImageOptions pullOptions = {};
auto pullOp = session.PullImageAsync(pullOptions);
pullOp.Progress([](auto&&, ImageProgress const& p) { /* progress */ });
co_await pullOp;

PushImageOptions pushOptions = {};
co_await session.PushImageAsync(pushOptions);

TagImageOptions tagOptions = {};
session.TagImage(tagOptions);

VhdOptions vhdOptions = {};
session.CreateVhdVolume(vhdOptions);
session.DeleteVhdVolume(L"build-cache");
```

```cpp
session.Terminate();
```
