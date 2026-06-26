# ImageProgress

Progress payload for pull/import/load/push operations.

```csharp
public sealed class ImageProgress
{
    public string Id { get; }
    public ImageProgressStatus Status { get; }
    public ulong CurrentBytes { get; }
    public ulong TotalBytes { get; }
}
```

Example:

```csharp
void PrintImageProgress(ImageProgress progress) =>
    Console.WriteLine($"{progress.Status,-12} {progress.Id} {progress.CurrentBytes}/{progress.TotalBytes}");
```
