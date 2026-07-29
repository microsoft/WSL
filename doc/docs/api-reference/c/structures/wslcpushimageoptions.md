# WslcPushImageOptions

```c
typedef struct WslcPushImageOptions
{
    _In_z_ PCSTR image;
    _In_z_ PCSTR registryAuth; // Base64-encoded X-Registry-Auth header value.
    _In_opt_ WslcContainerImageProgressCallback progressCallback;
    _In_opt_ PVOID progressCallbackContext;
} WslcPushImageOptions;
```

| Field | Type |
|---|---|
| `image` | `PCSTR` |
| `registryAuth` | `PCSTR` |
| `progressCallback` | `WslcContainerImageProgressCallback` |
| `progressCallbackContext` | `PVOID` |
