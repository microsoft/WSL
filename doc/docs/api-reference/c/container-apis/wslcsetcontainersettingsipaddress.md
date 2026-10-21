# WslcSetContainerSettingsIPAddress

```c
STDAPI WslcSetContainerSettingsIPAddress(_In_ WslcContainerSettings* containerSettings, _In_opt_ const IN_ADDR* ipAddress);
```

| Parameter | Type | Direction |
|---|---|---|
| `containerSettings` | `WslcContainerSettings*` | in |
| `ipAddress` | `const IN_ADDR*` | in, optional |

Return value: `HRESULT`.

Notes:

- Sets the container's IPv4 address on its primary network. Pass `NULL` to clear.
- Requires exactly one user-defined network set with [WslcSetContainerSettingsNetworks](wslcsetcontainersettingsnetworks.md); [WslcCreateContainer](wslccreatecontainer.md) returns `E_INVALIDARG` otherwise.
- The address is copied.

Example:

```c
IN_ADDR address = { 0 };
inet_pton(AF_INET, "172.30.0.10", &address);

HRESULT hr = WslcSetContainerSettingsIPAddress(&containerSettings, &address);
```
