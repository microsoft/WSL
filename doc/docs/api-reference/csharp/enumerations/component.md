# Component

```csharp
public enum Component
{
    VirtualMachinePlatform = 1,
    WslPackage = 2,
    SdkNeedsUpdate = 4
}
```

`SdkNeedsUpdate` indicates that the SDK bundled with the application is incompatible with the
installed runtime. The application must be updated; this value cannot be passed to
`WslcService.InstallWithDependencies`.
