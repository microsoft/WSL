# WslcLoadImageOptions

```c
typedef struct WslcLoadImageOptions
{
    _In_opt_ WslcContainerImageProgressCallback progressCallback;
    _In_opt_ PVOID progressCallbackContext;
} WslcLoadImageOptions;
```

| Field | Type |
|---|---|
| `progressCallback` | `WslcContainerImageProgressCallback` |
| `progressCallbackContext` | `PVOID` |
