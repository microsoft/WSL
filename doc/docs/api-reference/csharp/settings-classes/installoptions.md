# InstallOptions

Selects components for `WslcService.InstallWithDependencies` and controls repair behavior.

```csharp
public sealed class InstallOptions
{
    public InstallOptions();

    public IReadOnlyList<Component> Components { get; set; }
    public bool Repair { get; set; }
}
```

If `Components` is `null`, the service determines which components require installation. Set
`Repair` to `true` to allow the WSL package to be reinstalled; it has no effect when installing
Virtual Machine Platform. If the component list contains `Component.SdkNeedsUpdate`, installation
fails. Stop and tell the user to update the application.
