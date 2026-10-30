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

Example:

```c
WslcSession session = NULL;
HRESULT hr = WslcCreateSession(&sessionSettings, &session, NULL);
```
