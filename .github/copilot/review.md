## Code Review Guidelines for WSL

When reviewing code, enforce the conventions in `.github/copilot-instructions.md`. Focus especially on these high-risk areas:

### ABI Safety (Critical)
Only the SDK-facing and public surfaces require a backward-compatible ABI. Every other COM interface (`wslc.idl`, `wslservice.idl`, etc.) is internal and non-stable: shipped in lockstep with its only clients, with the proxy/stub regenerated each build, so methods may be appended to those freely. Do **not** flag internal, non-stable interfaces.
- **Flag** ABI-breaking changes (new, removed, or reordered methods, changed struct layouts) only in the stable surfaces: `WSLCCompat.idl` (the WSLC SDK-facing layer, which must stay backward compatible) and the public plugin API (`WSLPluginHooksV1` / `WSLPluginAPIV1` structs in `WslPluginApi.h`).
- **Do not flag** new methods appended to internal, non-stable interfaces such as `IWSLCSession` in `wslc.idl`, or interfaces in `wslservice.idl`.

### Resource Safety
- **Flag** raw `CloseHandle()`, `delete`, `free()`, or manual resource cleanup — require WIL smart pointers
- **Flag** missing `NON_COPYABLE()` / `NON_MOVABLE()` on classes that hold resources
- **Flag** lock usage without `_Guarded_by_()` SAL annotations

### User-Facing Changes
- **Flag** hardcoded English strings — require `Localization::MessageXxx()` and Resources.resw entry
- **Flag** new `.wslconfig` settings without corresponding Resources.resw localization string
- **Flag** silent fallback on invalid config values — require `EMIT_USER_WARNING()`

### Error Handling
- **Flag** bare `if (FAILED(hr))` — require WIL macros
- **Flag** silently swallowed errors — require `CATCH_LOG()` or `LOG_IF_FAILED()`
- **Flag** telemetry events missing privacy data tags
