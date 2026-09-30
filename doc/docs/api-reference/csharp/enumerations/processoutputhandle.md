# ProcessOutputHandle

Only stdout/stderr are modeled here. Stdin is accessed through `Process.GetInputStream()`.

```csharp
public enum ProcessOutputHandle
{
    StandardOutput = 1,
    StandardError = 2
}
```
