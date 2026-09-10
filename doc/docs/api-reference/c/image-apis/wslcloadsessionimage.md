# WslcLoadSessionImage

```c
STDAPI WslcLoadSessionImage(
    _In_ WslcSession session,
    _In_ HANDLE imageContent,
    _In_ uint64_t imageContentBytes,
    _In_opt_ const WslcLoadImageOptions* options,
    _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `imageContent` | `HANDLE` | in |
| `imageContentBytes` | `uint64_t` | in |
| `options` | `const WslcLoadImageOptions*` | in, optional |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Important: the header declares `imageContent` as `HANDLE`, not `void*`.

Example:

```c
HANDLE imageContent = CreateFileW(
    L"C:\\images\\demo-load.tar",
    GENERIC_READ,
    FILE_SHARE_READ,
    NULL,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    NULL);

LARGE_INTEGER size = { 0 };
GetFileSizeEx(imageContent, &size);

WslcLoadImageOptions loadOptions = { 0 };
HRESULT hr = WslcLoadSessionImage(
    session,
    imageContent,
    (uint64_t)size.QuadPart,
    &loadOptions,
    NULL);

CloseHandle(imageContent);
```
