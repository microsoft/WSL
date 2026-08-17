# WslcImportImageOptions

```c
typedef struct WslcImportImageOptions
{
    _In_opt_ WslcContainerImageProgressCallback progressCallback;
    _In_opt_ PVOID progressCallbackContext;
} WslcImportImageOptions;
```

| Field | Type |
|---|---|
| `progressCallback` | `WslcContainerImageProgressCallback` |
| `progressCallbackContext` | `PVOID` |
