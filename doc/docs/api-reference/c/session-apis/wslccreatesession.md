# WslcCreateSession

```c
STDAPI WslcCreateSession(_In_ WslcSessionSettings* sessionSettings, _Out_ WslcSession* session, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `sessionSettings` | `WslcSessionSettings*` | in |
| `session` | `WslcSession*` | out |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Returns [`WSLC_E_RUNTIME_UPDATE_NEEDED`](../error-codes.md) if the installed WSL runtime does not support the requested settings, such as host loopback, which requires WSL 3.0.5 or later.

Requesting `WSLC_SESSION_FEATURE_FLAG_LARGE_MEMORY_BACKING_PAGES` requires WSL 3.1.0 or later.
On an older runtime, returns `WSLC_E_RUNTIME_UPDATE_NEEDED` and, when provided, sets `errorMessage`
to a localized message containing the installed and required WSL versions.

Example:

```c
WslcSession session = NULL;
HRESULT hr = WslcCreateSession(&sessionSettings, &session, NULL);
```
