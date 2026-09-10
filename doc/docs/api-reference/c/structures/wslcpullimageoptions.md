# WslcPullImageOptions

```c
typedef struct WslcPullImageOptions
{
    _In_z_ PCSTR uri;
    WslcContainerImageProgressCallback progressCallback;
    PVOID progressCallbackContext;
    _In_opt_z_ PCSTR registryAuth;
} WslcPullImageOptions;
```

| Field | Type |
|---|---|
| `uri` | `PCSTR` |
| `progressCallback` | `WslcContainerImageProgressCallback` |
| `progressCallbackContext` | `PVOID` |
| `registryAuth` | `PCSTR` |
