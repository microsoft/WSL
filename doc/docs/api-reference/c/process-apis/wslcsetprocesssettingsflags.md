# WslcSetProcessSettingsFlags

```c
STDAPI WslcSetProcessSettingsFlags(_In_ WslcProcessSettings* processSettings, _In_ WslcProcessFlags flags);
```

| Parameter | Type | Direction |
|---|---|---|
| `processSettings` | `WslcProcessSettings*` | in |
| `flags` | [`WslcProcessFlags`](../enumerations/wslcprocessflags.md) | in |

Return value: `HRESULT`. Unknown flag bits are rejected with `E_INVALIDARG`.

Replaces any previously set flags. Must be called before the settings are used to start a
process ([WslcCreateContainerProcess](../container-apis/wslccreatecontainerprocess.md)) or a
container init process
([WslcSetContainerSettingsInitProcess](../container-apis/wslcsetcontainersettingsinitprocess.md)).

Standard input is **disabled by default**. Without `WSLC_PROCESS_FLAG_STDIN` the process
observes an immediately closed stdin and `WslcGetProcessIOHandle` cannot be used with
`WSLC_PROCESS_IO_HANDLE_STDIN`.

Example:

```c
HRESULT hr = WslcSetProcessSettingsFlags(&processSettings, WSLC_PROCESS_FLAG_STDIN);
```
