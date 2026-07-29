# WslcImportSessionImage

```c
STDAPI WslcImportSessionImage(
    _In_ WslcSession session,
    _In_z_ PCSTR imageName,
    _In_ HANDLE imageContent,
    _In_ uint64_t imageContentBytes,
    _In_opt_ const WslcImportImageOptions* options,
    _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `imageName` | `PCSTR` | in |
| `imageContent` | `HANDLE` | in |
| `imageContentBytes` | `uint64_t` | in |
| `options` | `const WslcImportImageOptions*` | in, optional |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Important: the header declares `imageContent` as `HANDLE`, not `void*`.

Example:

```c
HANDLE imageContent = CreateFileW(
    L"C:\\images\\demo-import.tar",
    GENERIC_READ,
    FILE_SHARE_READ,
    NULL,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    NULL);

LARGE_INTEGER size = { 0 };
GetFileSizeEx(imageContent, &size);

WslcImportImageOptions importOptions = { 0 };
HRESULT hr = WslcImportSessionImage(
    session,
    "demo/imported:latest",
    imageContent,
    (uint64_t)size.QuadPart,
    &importOptions,
    NULL);

CloseHandle(imageContent);
```
