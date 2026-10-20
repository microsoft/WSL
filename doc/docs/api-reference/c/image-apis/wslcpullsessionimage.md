# WslcPullSessionImage

```c
STDAPI WslcPullSessionImage(_In_ WslcSession session, _In_ const WslcPullImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `options` | `const WslcPullImageOptions*` | in |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Example:

```c
HRESULT CALLBACK OnImageProgress(const WslcImageProgressMessage* progress, PVOID context)
{
    UNREFERENCED_PARAMETER(context);
    printf("%s %llu/%llu\n",
        progress->id,
        (unsigned long long)progress->detail.currentBytes,
        (unsigned long long)progress->detail.totalBytes);
    return S_OK;
}

WslcPullImageOptions pullOptions = { 0 };
pullOptions.uri = "docker.io/library/alpine:latest";
pullOptions.progressCallback = OnImageProgress;
pullOptions.progressCallbackContext = NULL;
pullOptions.registryAuth = NULL;

HRESULT hr = WslcPullSessionImage(session, &pullOptions, NULL);
```
