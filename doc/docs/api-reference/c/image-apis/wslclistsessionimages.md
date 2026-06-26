# WslcListSessionImages

```c
STDAPI WslcListSessionImages(_In_ WslcSession session, _Outptr_result_buffer_(*count) WslcImageInfo** images, _Out_ uint32_t* count);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `images` | `WslcImageInfo**` | out |
| `count` | `uint32_t*` | out |

Return value: `HRESULT`.

Header note: `images` is allocated using `CoTaskMemAlloc`; free it with `CoTaskMemFree`.

Example:

```c
WslcImageInfo* images = NULL;
uint32_t count = 0;
HRESULT hr = WslcListSessionImages(session, &images, &count);

if (SUCCEEDED(hr))
{
    for (uint32_t i = 0; i < count; ++i)
    {
        printf("%s\n", images[i].name);
    }
    CoTaskMemFree(images);
}
```
