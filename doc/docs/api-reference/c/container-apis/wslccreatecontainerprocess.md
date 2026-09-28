# WslcCreateContainerProcess

```c
STDAPI WslcCreateContainerProcess(
    _In_ WslcContainer container, _In_ WslcProcessSettings* newProcessSettings, _Out_ WslcProcess* newProcess, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `container` | `WslcContainer` | in |
| `newProcessSettings` | `WslcProcessSettings*` | in |
| `newProcess` | `WslcProcess*` | out |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Example:

```c
WslcProcessSettings processSettings;
WslcProcess process = NULL;
PCSTR const argv[] = { "/bin/echo", "hello from wscl" };

WslcInitProcessSettings(&processSettings);
WslcSetProcessSettingsCmdLine(&processSettings, argv, _countof(argv));

HRESULT hr = WslcCreateContainerProcess(container, &processSettings, &process, NULL);
```
