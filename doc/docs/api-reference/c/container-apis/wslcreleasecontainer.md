# WslcReleaseContainer

```c
STDAPI WslcReleaseContainer(_In_ WslcContainer container);
```

| Parameter | Type | Direction |
|---|---|---|
| `container` | `WslcContainer` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcReleaseContainer(container);
container = NULL;
```

---
