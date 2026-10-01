# WslcSetSessionSettingsVhd

```c
STDAPI WslcSetSessionSettingsVhd(_In_ WslcSessionSettings* sessionSettings, _In_opt_ const WslcVhdRequirements* vhdRequirements);
```

| Parameter | Type | Direction |
|---|---|---|
| `sessionSettings` | `WslcSessionSettings*` | in |
| `vhdRequirements` | `const WslcVhdRequirements*` | in, optional |

Return value: `HRESULT`.

Header notes:

- `flags` must be `WSLC_VHD_REQ_FLAG_NONE`; other flags are rejected with `E_INVALIDARG`.
- Session storage supports `WSLC_VHD_TYPE_DYNAMIC` and `WSLC_VHD_TYPE_SPARSE`.
- `WSLC_VHD_TYPE_SPARSE` is currently experimental.
- Creating a session with `WSLC_VHD_TYPE_SPARSE` returns `WSLC_E_WSL_UPDATE_NEEDED` when the installed WSL version does not support sparse VHDs.

Example:

```c
WslcVhdRequirements vhdRequirements = { 0 };
vhdRequirements.name = "ignored-by-WslcSetSessionSettingsVhd";
vhdRequirements.sizeBytes = (uint64_t)64 * 1024 * 1024 * 1024;
vhdRequirements.type = WSLC_VHD_TYPE_DYNAMIC;
vhdRequirements.flags = WSLC_VHD_REQ_FLAG_NONE;
vhdRequirements.uid = (uint32_t)0;
vhdRequirements.gid = (uint32_t)0;

HRESULT hr = WslcSetSessionSettingsVhd(&sessionSettings, &vhdRequirements);
```
