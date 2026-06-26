# WslcService

Static entry point for service-level operations.

```csharp
public static class WslcService
{
    public static ComponentFlags GetMissingComponents();
    public static ServiceVersion GetVersion();
    public static IAsyncActionWithProgress<InstallProgress> InstallWithDependenciesAsync();
}
```

## WslcService.GetMissingComponents()

```csharp
ComponentFlags missing = WslcService.GetMissingComponents();
if (missing == ComponentFlags.None)
{
    Console.WriteLine("All required components are installed.");
}
else
{
    Console.WriteLine($"Missing: {missing}");
}
```

## WslcService.GetVersion()

```csharp
ServiceVersion version = WslcService.GetVersion();
Console.WriteLine($"{version.Major}.{version.Minor}.{version.Revision}");
```

## WslcService.InstallWithDependenciesAsync()

```csharp
var install = WslcService.InstallWithDependenciesAsync();
install.Progress = (op, progress) =>
    Console.WriteLine($"install: {progress.Component} {progress.Progress}/{progress.Total}");
await install;
```

---
