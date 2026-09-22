# InstallOptions

Selects components for `WslcService::InstallWithDependencies` and controls repair behavior.

**Properties**

- `Components()` / setter
- `Repair()` / setter

If `Components` is null, the service determines which components require installation. If the
result includes `Component::SdkNeedsUpdate`, the installation fails because the running SDK cannot
update itself. Update the client package and exclude that value before installing other components.

Set `Repair` to `true` to allow selected components to be reinstalled.
