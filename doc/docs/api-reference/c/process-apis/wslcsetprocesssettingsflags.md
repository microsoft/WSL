# WslcSetProcessSettingsFlags

```c
STDAPI WslcSetProcessSettingsFlags(_In_ WslcProcessSettings* processSettings, _In_ WslcProcessFlags flags);
```

| Parameter | Type | Direction |
|---|---|---|
| `processSettings` | `WslcProcessSettings*` | in |
| `flags` | [`WslcProcessFlags`](../enumerations/wslcprocessflags.md) | in |

Return value: `HRESULT`. Unknown flag bits are rejected with `E_INVALIDARG`.

Replaces any previously set flags. Must be called before
[WslcCreateContainerProcess](../container-apis/wslccreatecontainerprocess.md)
or before [WslcCreateContainer](../container-apis/wslccreatecontainer.md) for a
container init process.

Standard input is **disabled by default**. Without `WSLC_PROCESS_FLAG_STDIN` the process
observes an immediately closed stdin and `WslcGetProcessIOHandle` cannot be used with
`WSLC_PROCESS_IO_HANDLE_STDIN`.

Example:

```c
HRESULT hr = WslcSetProcessSettingsFlags(&processSettings, WSLC_PROCESS_FLAG_STDIN);
```
