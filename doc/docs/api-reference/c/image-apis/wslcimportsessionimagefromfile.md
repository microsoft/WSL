# WslcImportSessionImageFromFile

```c
STDAPI WslcImportSessionImageFromFile(
    _In_ WslcSession session, _In_z_ PCSTR imageName, _In_z_ PCWSTR path, _In_opt_ const WslcImportImageOptions* options, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `imageName` | `PCSTR` | in |
| `path` | `PCWSTR` | in |
| `options` | `const WslcImportImageOptions*` | in, optional |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Example:

```c
WslcImportImageOptions importOptions = { 0 };
HRESULT hr = WslcImportSessionImageFromFile(
    session,
    "demo/imported:latest",
    L"C:\\images\\demo-import.tar",
    &importOptions,
    NULL);
```
