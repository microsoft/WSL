# Component

`WslcService::GetMissingComponents()` returns a view of missing components.

| Enumerator | Meaning |
|---|---|
| `VirtualMachinePlatform` | The Virtual Machine Platform optional component is not enabled. |
| `WslPackage` | The WSL package is not installed or must be updated to a version that supports WSLC. |
| `SdkNeedsUpdate` | The application uses an incompatible SDK version and must be updated. |
