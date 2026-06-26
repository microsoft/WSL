# WslcDeleteSessionImage

```c
STDAPI WslcDeleteSessionImage(_In_ WslcSession session, _In_z_ PCSTR nameOrID, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `nameOrID` | `PCSTR` | in |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcDeleteSessionImage(session, "demo/imported:latest", NULL);
```
