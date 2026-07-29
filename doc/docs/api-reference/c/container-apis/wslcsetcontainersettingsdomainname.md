# WslcSetContainerSettingsDomainName

```c
STDAPI WslcSetContainerSettingsDomainName(_In_ WslcContainerSettings* containerSettings, _In_ PCSTR domainName);
```

| Parameter | Type | Direction |
|---|---|---|
| `containerSettings` | `WslcContainerSettings*` | in |
| `domainName` | `PCSTR` | in |

Return value: `HRESULT`.

Example:

```c
HRESULT hr = WslcSetContainerSettingsDomainName(&containerSettings, "example.internal");
```
