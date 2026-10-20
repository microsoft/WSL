# Session

Represents a WSL-backed container host session.

## Session constructor

The settings object must not be null.

```cpp
Session session{ sessionSettings };
```

## Session::Start()

Starts the session. Calling this method more than once throws an exception.

```cpp
session.Start();
```

## Session::Terminate()

Terminates the session.

```cpp
session.Terminate();
```

## Session::CreateContainer(ContainerSettings)

Creates a container object owned by the session.

```cpp
auto container = session.CreateContainer(containerSettings);
```

## Session::OpenContainer(hstring, ProcessOutputMode)

Opens an existing container by name, full ID, or unambiguous partial ID prefix. The output mode
controls how the opened container's init-process I/O is exposed when it is started.

```cpp
auto opened = session.OpenContainer(L"demo-container", ProcessOutputMode::Event);
```

The method throws a projected exception with `Error::ContainerNotFound` if no container matches or
`Error::ContainerPrefixAmbiguous` if a partial ID is ambiguous.

## Session::PullImage(PullImageOptions)

Pulls an image synchronously.

```cpp
session.PullImage(PullImageOptions{ L"docker.io/library/alpine:latest" });
```

## Session::PullImageAsync(PullImageOptions)

Pulls an image asynchronously and reports progress.

```cpp
auto pullOp = session.PullImageAsync(PullImageOptions{ L"docker.io/library/alpine:latest" });
pullOp.Progress([](auto&&, ImageProgress const& progress)
{
    printf("pull status: %d\n", static_cast<int>(progress.Status()));
});
co_await pullOp;
```

## Session::ImportImage(hstring, hstring)

Imports an image tarball synchronously from a file path.

```cpp
session.ImportImage(L"C:\\images\\alpine.tar", L"demo/alpine:latest");
```

## Session::ImportImageAsync(hstring, hstring)

Imports an image tarball asynchronously from a file path.

```cpp
auto importOp = session.ImportImageAsync(L"C:\\images\\alpine.tar", L"demo/alpine:latest");
importOp.Progress([](auto&&, ImageProgress const& p) { /* progress */ });
co_await importOp;
```

## Session::LoadImage(hstring)

Loads an image archive synchronously from a file path.

```cpp
session.LoadImage(L"C:\\images\\bundle.tar");
```

## Session::LoadImageAsync(hstring)

Loads an image archive asynchronously from a file path.

```cpp
auto loadOp = session.LoadImageAsync(L"C:\\images\\bundle.tar");
co_await loadOp;
```

## Session::PushImage(PushImageOptions)

Pushes an image synchronously to a registry.

```cpp
session.PushImage(pushOptions);
```

## Session::PushImageAsync(PushImageOptions)

Pushes an image asynchronously to a registry.

```cpp
co_await session.PushImageAsync(pushOptions);
```

## Session::DeleteImage(hstring)

Deletes an image by name or ID.

```cpp
session.DeleteImage(L"demo/alpine:latest");
```

## Session::TagImage(TagImageOptions)

Applies a new repository and tag to an existing image.

```cpp
session.TagImage(TagImageOptions{ L"alpine:latest", L"registry.example.com/alpine", L"v1" });
```

## Session::CreateVhdVolume(VhdOptions)

Creates a named session VHD volume.

```cpp
session.CreateVhdVolume(vhdOptions);
```

## Session::DeleteVhdVolume(hstring)

Deletes a named session VHD volume.

```cpp
session.DeleteVhdVolume(L"build-cache");
```

## Session::Authenticate(Uri, hstring, hstring)

Authenticates to a registry and returns registry authentication data suitable for
`PullImageOptions::RegistryAuth()` or `PushImageOptions::RegistryAuth()`.

```cpp
auto authentication = session.Authenticate(
    winrt::Windows::Foundation::Uri{ L"https://registry.example.com" },
    L"user",
    L"password");

PullImageOptions authenticatedPull{ L"registry.example.com/demo:latest" };
authenticatedPull.RegistryAuth(authentication.IdentityToken());
session.PullImage(authenticatedPull);
```

## Session::GetImages()

Returns a snapshot of images known to the session.

```cpp
auto images = session.GetImages();
for (auto const& image : images)
{
    printf("%ws\n", image.Name().c_str());
}
```

## Session::Terminated event

Raised when the session termination event is signaled.

```cpp
session.Terminated([](SessionTerminationReason reason)
{
    printf("session terminated: %d\n", static_cast<int>(reason));
});
```

## Session::ProcessCrashed event

Raised when a process crash dump is reported.

```cpp
session.ProcessCrashed([](ProcessCrashInformation const& information)
{
    printf("process crashed: %ws (%u)\n", information.ProcessName().c_str(), information.Pid());
});
```

## Session::Close()

Releases the underlying WinRT session object.

```cpp
session.Close();
```
