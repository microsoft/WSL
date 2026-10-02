# WslcService

Static entry point for service-level operations.

```csharp
public static class WslcService
{
    public static IReadOnlyList<Component> GetMissingComponents();
    public static ServiceVersion GetVersion();
    public static void InstallWithDependencies(InstallOptions options);
    public static IAsyncActionWithProgress<InstallProgress> InstallWithDependenciesAsync(InstallOptions options);
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

## WslcService.InstallWithDependencies(InstallOptions)

```csharp
IReadOnlyList<Component> missing = WslcService.GetMissingComponents();
if (missing.Contains(Component.SdkNeedsUpdate))
{
    // Installing components cannot resolve this compatibility error.
    return;
}

var options = new InstallOptions
{
    Components = missing
};

if (missing.Count != 0)
{
    WslcService.InstallWithDependencies(options);
}
```

## WslcService.InstallWithDependenciesAsync(InstallOptions)

```csharp
IReadOnlyList<Component> missing = WslcService.GetMissingComponents();
if (missing.Contains(Component.SdkNeedsUpdate))
{
    // Installing components cannot resolve this compatibility error.
    return;
}

var options = new InstallOptions
{
    Components = missing
};

if (missing.Count != 0)
{
    var install = WslcService.InstallWithDependenciesAsync(options);
    install.Progress = (op, progress) =>
        Console.WriteLine($"install: {progress.Component} {progress.Progress}/{progress.Total}");
    await install;
}
```

---
