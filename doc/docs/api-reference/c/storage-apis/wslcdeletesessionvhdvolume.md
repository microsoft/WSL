# WslcDeleteSessionVhdVolume

```c
STDAPI WslcDeleteSessionVhdVolume(_In_ WslcSession session, _In_z_ PCSTR name, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `name` | `PCSTR` | in |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcDeleteSessionVhdVolume(session, "cache", NULL);
```

---
