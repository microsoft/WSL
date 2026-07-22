# WslcInitSessionSettings

```c
STDAPI WslcInitSessionSettings(_In_ PCWSTR name, _In_ PCWSTR storagePath, _Out_ WslcSessionSettings* sessionSettings);
```

| Parameter | Type | Direction | Comment |
|---|---|---|---|
| `name` | `PCWSTR` | in | The name of the session to be created. |
| `storagePath` | `PCWSTR` | in | Path to where the session storage should be written. If the path doesn't exist, it will be created. |
| `sessionSettings` | `WslcSessionSettings*` | out | Pointer to the `WslcSessionSettings` to write the settings to. |

Return value: `HRESULT`.

Session names serve both as display names and as machine-wide keys used to identify sessions. If a session with the same name already exists, session creation will fail with `ERROR_ALREADY_EXISTS`.

Also note that the following information about a session is visible to all users on the machine:

- The session's name
- The SID of the user that created the session
- The PID of the process that created the session


Do not put credentials or other sensitive information in the session's name.

Example:

```c
WslcSessionSettings sessionSettings;
HRESULT hr = WslcInitSessionSettings(
    L"demo-session",
    L"C:\\WSLC\\demo-session",
    &sessionSettings);
```
