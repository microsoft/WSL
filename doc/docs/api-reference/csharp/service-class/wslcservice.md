# WslcService

Static entry point for service-level operations.

```csharp
public static class WslcService
{
    public static IReadOnlyList<Component> GetMissingComponents();
    public static ServiceVersion GetVersion();
    public static void InstallWithDependencies();
    public static IAsyncActionWithProgress<InstallProgress> InstallWithDependenciesAsync();
}
```

## WslcService.GetMissingComponents()

```csharp
IReadOnlyList<Component> missing = WslcService.GetMissingComponents();
if (missing.Count == 0)
{
    Console.WriteLine("All required components are installed.");
}
else
{
    Console.WriteLine($"Missing: {string.Join(", ", missing)}");
}
```

## WslcService.GetVersion()

```csharp
ServiceVersion version = WslcService.GetVersion();
Console.WriteLine($"{version.Major}.{version.Minor}.{version.Revision}");
```

## WslcService.InstallWithDependencies()

```csharp
WslcService.InstallWithDependencies();
```

## WslcService.InstallWithDependenciesAsync()

```csharp
var install = WslcService.InstallWithDependenciesAsync();
install.Progress = (op, progress) =>
    Console.WriteLine($"install: {progress.Component} {progress.Progress}/{progress.Total}");
await install;
```

---
