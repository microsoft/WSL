# WslcInstallCallback

```c
typedef __callback void(CALLBACK* WslcInstallCallback)(
    _In_ WslcComponentFlags component, _In_ uint32_t progressSteps, _In_ uint32_t totalSteps, _In_opt_ PVOID context);
```

| Parameter | Type |
|---|---|
| `component` | `WslcComponentFlags` |
| `progressSteps` | `uint32_t` |
| `totalSteps` | `uint32_t` |
| `context` | `PVOID` |

---
