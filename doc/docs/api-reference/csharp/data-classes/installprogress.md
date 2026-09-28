# InstallProgress

Progress payload for dependency installation.

```csharp
public sealed class InstallProgress
{
    public Component Component { get; }
    public uint Progress { get; }
    public uint Total { get; }
}
```

Example:

```csharp
void PrintInstallProgress(InstallProgress progress) =>
    Console.WriteLine($"{progress.Component}: {progress.Progress}/{progress.Total}");
```
