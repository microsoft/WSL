# WslcLoadSessionImageFromFile

```c
STDAPI WslcLoadSessionImageFromFile(
    _In_ WslcSession session, _In_z_ PCWSTR path, _In_opt_ const WslcLoadImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `path` | `PCWSTR` | in |
| `options` | `const WslcLoadImageOptions*` | in, optional |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Example:

```c
WslcLoadImageOptions loadOptions = { 0 };
HRESULT hr = WslcLoadSessionImageFromFile(
    session,
    L"C:\\images\\demo-load.tar",
    &loadOptions,
    NULL);
```
