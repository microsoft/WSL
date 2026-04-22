## Code Review Guidelines for WSL

When reviewing code, enforce the conventions in `.github/copilot-instructions.md`. Focus especially on these high-risk areas:

### ABI Safety (Critical)
- **Flag** new methods added to existing COM interfaces without a new versioned interface/IID
- **Flag** changed struct layouts in IDL files
- **Flag** changes to `WSLPluginHooksV1` or `WSLPluginAPIV1` structs (public API)

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
