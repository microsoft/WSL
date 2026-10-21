# WslcCreateContainer

```c
STDAPI WslcCreateContainer(_In_ WslcSession session, _In_ const WslcContainerSettings* containerSettings, _Out_ WslcContainer* container, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `containerSettings` | `const WslcContainerSettings*` | in |
| `container` | `WslcContainer*` | out |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`. Returns `E_INVALIDARG` if:

- Both [WslcSetContainerSettingsNetworkingMode](wslcsetcontainersettingsnetworkingmode.md) and [WslcSetContainerSettingsNetworks](wslcsetcontainersettingsnetworks.md) were called.
- An IP address was set with [WslcSetContainerSettingsIPAddress](wslcsetcontainersettingsipaddress.md) without exactly one network, or on a primary network that is not user-defined.
- Aliases are set on a network that is not user-defined.

Returns [`WSLC_E_RUNTIME_UPDATE_NEEDED`](../error-codes.md) if the installed WSL runtime does not support the requested settings, such as capabilities, aliases on the primary network, or an IP address.

Example:

```c
WslcContainer container = NULL;
HRESULT hr = WslcCreateContainer(session, &containerSettings, &container, NULL);
```
