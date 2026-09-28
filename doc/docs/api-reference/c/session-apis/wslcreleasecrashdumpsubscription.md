# WslcReleaseCrashDumpSubscription

```c
STDAPI WslcReleaseCrashDumpSubscription(_In_ WslcCrashDumpSubscription subscription);
```

| Parameter | Type | Direction |
|---|---|---|
| `subscription` | `WslcCrashDumpSubscription` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcReleaseCrashDumpSubscription(subscription);
subscription = NULL;
```
