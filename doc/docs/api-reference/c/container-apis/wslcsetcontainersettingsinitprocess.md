# WslcSetContainerSettingsInitProcess

```c
STDAPI WslcSetContainerSettingsInitProcess(_In_ WslcContainerSettings* containerSettings, _In_ WslcProcessSettings* initProcess);
```

| Parameter | Type | Direction |
|---|---|---|
| `containerSettings` | `WslcContainerSettings*` | in |
| `initProcess` | `WslcProcessSettings*` | in |

Return value: `HRESULT`.

Example:

```c
WslcProcessSettings initProcess;
PCSTR const argv[] = { "/bin/sh", "-c", "sleep 3600" };

WslcInitProcessSettings(&initProcess);
WslcSetProcessSettingsCmdLine(&initProcess, argv, _countof(argv));

HRESULT hr = WslcSetContainerSettingsInitProcess(&containerSettings, &initProcess);
```
