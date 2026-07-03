# WslcGetVersion

```c
STDAPI WslcGetVersion(_Out_writes_(1) WslcVersion* version);
```

| Parameter | Type | Direction |
|---|---|---|
| `version` | `WslcVersion*` | out |

Return value: `HRESULT`.

Example:

```c
WslcVersion version = { 0 };
HRESULT hr = WslcGetVersion(&version);
if (SUCCEEDED(hr))
{
    printf("%u.%u.%u\n", version.major, version.minor, version.revision);
}
```
