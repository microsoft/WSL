# Session

Represents a WSL-backed container host session.

```csharp
public sealed class Session : IDisposable
{
    public Session(SessionSettings settings);

    public event SessionTerminationHandler Terminated;
    public event ProcessCrashHandler ProcessCrashed;

    public void Start();
    public void Terminate();
    public Container CreateContainer(ContainerSettings containerSettings);
    public void PullImage(PullImageOptions options);
    public IAsyncActionWithProgress<ImageProgress> PullImageAsync(PullImageOptions options);
    public void ImportImage(string path, string imageName);
    public IAsyncActionWithProgress<ImageProgress> ImportImageAsync(string path, string imageName);
    public void LoadImage(string path);
    public IAsyncActionWithProgress<ImageProgress> LoadImageAsync(string path);
    public void PushImage(PushImageOptions options);
    public IAsyncActionWithProgress<ImageProgress> PushImageAsync(PushImageOptions options);
    public void DeleteImage(string nameOrId);
    public void TagImage(TagImageOptions options);
    public void CreateVhdVolume(VhdOptions options);
    public void DeleteVhdVolume(string name);
    public string Authenticate(Uri serverAddress, string username, string password);
    public IReadOnlyList<ImageInfo> GetImages();
    public void Dispose();
}
```

## Session constructor

```csharp
var session = new Session(sessionSettings);
```

## Session.Start()

Starts the session VM and registers the internal termination wait.

```csharp
session.Start();
```

## Session.Terminate()

Terminates the session.

```csharp
session.Terminate();
```

## Session.CreateContainer(ContainerSettings)

Creates a container object owned by the session.

```csharp
Container container = session.CreateContainer(containerSettings);
```

## Session.PullImage(PullImageOptions)

Synchronous image pull.

```csharp
session.PullImage(new PullImageOptions("docker.io/library/alpine:latest"));
```

## Session.PullImageAsync(PullImageOptions)

Awaitable pull with progress.

```csharp
var pull = session.PullImageAsync(new PullImageOptions("docker.io/library/alpine:latest"));
pull.Progress = (op, progress) =>
    Console.WriteLine($"pull: {progress.Status} {progress.Id} {progress.CurrentBytes}/{progress.TotalBytes}");
await pull;
```

## Session.ImportImage(string path, string imageName)

Synchronous image import from a file path.

```csharp
session.ImportImage(@"C:\images\demo.tar", "demo:imported");
```

## Session.ImportImageAsync(string path, string imageName)

Imports an image tarball from a file path.

```csharp
var importOp = session.ImportImageAsync(@"C:\images\demo.tar", "demo:imported");
importOp.Progress = (op, progress) =>
    Console.WriteLine($"import: {progress.Status} {progress.Id}");
await importOp;
```

## Session.LoadImage(string path)

Synchronous image load from disk.

```csharp
session.LoadImage(@"C:\images\docker-save.tar");
```

## Session.LoadImageAsync(string path)

Loads an image archive from disk.

```csharp
var loadOp = session.LoadImageAsync(@"C:\images\docker-save.tar");
loadOp.Progress = (op, progress) =>
    Console.WriteLine($"load: {progress.Status} {progress.Id}");
await loadOp;
```

## Session.PushImage(PushImageOptions)

Synchronous image push to a registry.

```csharp
session.PushImage(new PushImageOptions("registry.example.com/demo:latest", authToken));
```

## Session.PushImageAsync(PushImageOptions)

Pushes an image to a registry.

```csharp
var pushOp = session.PushImageAsync(new PushImageOptions("registry.example.com/demo:latest", authToken));
pushOp.Progress = (op, progress) =>
    Console.WriteLine($"push: {progress.Status} {progress.Id}");
await pushOp;
```

## Session.DeleteImage(string nameOrId)

Deletes an image by name or ID.

```csharp
session.DeleteImage("demo:old");
```

## Session.TagImage(TagImageOptions)

Applies a new repository/tag to an existing image.

```csharp
session.TagImage(new TagImageOptions("alpine:latest", "registry.example.com/alpine", "v1"));
```

## Session.CreateVhdVolume(VhdOptions)

Creates a named session VHD volume.

```csharp
var vhd = new VhdOptions("cache", 2UL * 1024 * 1024 * 1024, VhdType.Dynamic)
{
    Owner = new VhdOwner { Uid = 1000, Gid = 1000 }
};
session.CreateVhdVolume(vhd);
```

## Session.DeleteVhdVolume(string name)

Deletes a named session VHD volume.

```csharp
session.DeleteVhdVolume("cache");
```

## Session.Authenticate(Uri, string, string)

Authenticates to a registry and returns an identity token string.

```csharp
string token = session.Authenticate(
    new Uri("https://registry.example.com"),
    "user1",
    "password");
```

## Session.GetImages()

Returns a snapshot of images known to the session.

```csharp
foreach (var image in session.GetImages())
{
    Console.WriteLine(image.Name);
}
```

## Session.Terminated event

Raised when the session termination event is signaled.

```csharp
session.Terminated += reason =>
    Console.WriteLine($"Session terminated: {reason}");
```

## Session.ProcessCrashed event

Raised when a process crash dump is reported.

```csharp
session.ProcessCrashed += information =>
    Console.WriteLine($"Process crashed: {information.ProcessName} ({information.Pid})");
```

## Session.Dispose()

Releases the underlying WinRT session object.

```csharp
session.Dispose();
```
