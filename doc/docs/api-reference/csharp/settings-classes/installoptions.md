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
`Repair` to `true` to allow selected components to be reinstalled. If the component list contains
`Component.SdkNeedsUpdate`, installation fails because the running SDK cannot update itself. Update
the client package and exclude that value before installing other components.
