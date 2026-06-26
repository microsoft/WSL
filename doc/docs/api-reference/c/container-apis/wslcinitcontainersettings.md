# WslcInitContainerSettings

```c
STDAPI WslcInitContainerSettings(_In_ PCSTR imageName, _Out_ WslcContainerSettings* containerSettings);
```

| Parameter | Type | Direction |
|---|---|---|
| `imageName` | `PCSTR` | in |
| `containerSettings` | `WslcContainerSettings*` | out |

Return value: `HRESULT`.

Example:

```c
WslcContainerSettings containerSettings;
HRESULT hr = WslcInitContainerSettings("docker.io/library/alpine:latest", &containerSettings);
```
