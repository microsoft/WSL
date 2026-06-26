# ImageProgress

Progress payload for pull/import/load/push operations.

```csharp
public sealed class ImageProgress
{
    public string Id { get; set; }
    public ImageProgressStatus Status { get; set; }
    public ulong CurrentBytes { get; set; }
    public ulong TotalBytes { get; set; }
}
```

Example:

```csharp
void PrintImageProgress(ImageProgress progress) =>
    Console.WriteLine($"{progress.Status,-12} {progress.Id} {progress.CurrentBytes}/{progress.TotalBytes}");
```
