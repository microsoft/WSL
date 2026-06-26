# WslcPushSessionImage

```c
STDAPI WslcPushSessionImage(_In_ WslcSession session, _In_ const WslcPushImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `options` | `const WslcPushImageOptions*` | in |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Example:

```c
WslcPushImageOptions pushOptions = { 0 };
pushOptions.image = "demo/alpine:stable";
pushOptions.registryAuth = "BASE64_X_REGISTRY_AUTH";
pushOptions.progressCallback = OnImageProgress;
pushOptions.progressCallbackContext = NULL;

HRESULT hr = WslcPushSessionImage(session, &pushOptions, NULL);
```

---
