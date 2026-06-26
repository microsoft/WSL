# WslcReleaseSession

```c
STDAPI WslcReleaseSession(_In_ WslcSession session);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcReleaseSession(session);
session = NULL;
```
