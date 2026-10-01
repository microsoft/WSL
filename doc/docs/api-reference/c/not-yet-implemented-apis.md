# Not Yet Implemented APIs

The following APIs or features are **not yet implemented** and will return **`E_NOTIMPL`** when called. This list is provided so callers can plan accordingly:

| API / Feature | Details |
|---|---|
| **`WslcSetContainerSettingsPortMappings`** — UDP protocol | Only **TCP** (`WSLC_PORT_PROTOCOL_TCP`) is supported. Passing `WSLC_PORT_PROTOCOL_UDP` returns `E_NOTIMPL`. |
| **`WslcSetSessionSettingsVhd`** — fixed VHD type | Session storage supports `WSLC_VHD_TYPE_DYNAMIC` and `WSLC_VHD_TYPE_SPARSE`. Passing `WSLC_VHD_TYPE_FIXED` returns `E_NOTIMPL`; fixed VHDs are supported for named volumes through `WslcCreateSessionVhdVolume`. |

---
