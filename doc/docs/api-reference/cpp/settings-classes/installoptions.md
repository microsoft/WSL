# InstallOptions

Selects components for `WslcService::InstallWithDependencies` and controls repair behavior.

**Properties**

| Property | Type |
|---|---|
| `Components()` / setter | `IVectorView<Component>` |
| `Repair()` / setter | `bool` |

If `Components` is null, the service determines which components require installation. If the
result includes `Component::SdkNeedsUpdate`, installation fails because installing components
cannot resolve the SDK compatibility error.

Set `Repair` to `true` to allow the WSL package to be reinstalled. This setting has no effect when
installing Virtual Machine Platform.
