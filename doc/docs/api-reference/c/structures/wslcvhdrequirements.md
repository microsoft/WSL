# WslcVhdRequirements

```c
typedef struct WslcVhdRequirements
{
    _In_z_ PCSTR name;
    _In_ uint64_t sizeBytes; // Desired size (for create/expand)
    _In_ WslcVhdType type;
    _In_ WslcVhdRequirementsFlags flags;
    _In_ uint32_t uid; // honored iff (flags & WSLC_VHD_REQ_FLAG_OWNER)
    _In_ uint32_t gid; // honored iff (flags & WSLC_VHD_REQ_FLAG_OWNER)
} WslcVhdRequirements;
```

| Field | Type |
|---|---|
| `name` | `PCSTR` |
| `sizeBytes` | `uint64_t` |
| `type` | `WslcVhdType` |
| `flags` | `WslcVhdRequirementsFlags` |
| `uid` | `uint32_t` |
| `gid` | `uint32_t` |

Header notes:

- `name` is ignored by `WslcSetSessionSettingsVhd`.
- The remaining fields after `type` are only honored by `WslcCreateSessionVhdVolume`.
- `WslcSetSessionSettingsVhd` rejects non-`NONE` flags with `E_INVALIDARG`.
