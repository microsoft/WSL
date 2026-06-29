# WslcCreateSessionVhdVolume

```c
STDAPI WslcCreateSessionVhdVolume(_In_ WslcSession session, _In_ const WslcVhdRequirements* options, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `options` | `const WslcVhdRequirements*` | in |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Header notes:

- `WSLC_VHD_TYPE_FIXED` is only honored by `WslcCreateSessionVhdVolume`.
- `uid` and `gid` are honored iff `flags & WSLC_VHD_REQ_FLAG_OWNER`.

Example:

```c
WslcVhdRequirements options = { 0 };
options.name = "cache";
options.sizeBytes = (uint64_t)8 * 1024 * 1024 * 1024;
options.type = WSLC_VHD_TYPE_DYNAMIC;
options.flags = WSLC_VHD_REQ_FLAG_OWNER;
options.uid = (uint32_t)1000;
options.gid = (uint32_t)1000;

HRESULT hr = WslcCreateSessionVhdVolume(session, &options, NULL);
```
