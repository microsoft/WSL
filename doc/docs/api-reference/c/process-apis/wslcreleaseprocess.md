# WslcReleaseProcess

```c
STDAPI WslcReleaseProcess(_In_ WslcProcess process);
```

| Parameter | Type | Direction |
|---|---|---|
| `process` | `WslcProcess` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcReleaseProcess(process);
process = NULL;
```

---
