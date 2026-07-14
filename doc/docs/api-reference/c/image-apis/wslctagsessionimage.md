# WslcTagSessionImage

```c
STDAPI WslcTagSessionImage(_In_ WslcSession session, _In_ const WslcTagImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `options` | `const WslcTagImageOptions*` | in |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Example:

```c
WslcTagImageOptions tagOptions = { 0 };
tagOptions.image = "docker.io/library/alpine:latest";
tagOptions.repo = "demo/alpine";
tagOptions.tag = "stable";

HRESULT hr = WslcTagSessionImage(session, &tagOptions, NULL);
```
