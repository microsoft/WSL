# WslcInspectContainer

```c
STDAPI WslcInspectContainer(_In_ WslcContainer container, _Outptr_result_z_ PSTR* inspectData);
```

| Parameter | Type | Direction |
|---|---|---|
| `container` | `WslcContainer` | in |
| `inspectData` | `PSTR*` | out |

Return value: `HRESULT`.

Header note: `inspectData` is allocated using `CoTaskMemAlloc`; free it with `CoTaskMemFree`.

Example:

```c
PSTR inspectData = NULL;
HRESULT hr = WslcInspectContainer(container, &inspectData);
if (SUCCEEDED(hr))
{
    puts(inspectData);
    CoTaskMemFree(inspectData);
}
```
