# WslcSetProcessSettingsCmdLine

```c
STDAPI WslcSetProcessSettingsCmdLine(_In_ WslcProcessSettings* processSettings, _In_reads_(argc) PCSTR const* argv, size_t argc);
```

| Parameter | Type | Direction |
|---|---|---|
| `processSettings` | `WslcProcessSettings*` | in |
| `argv` | `PCSTR const*` | in |
| `argc` | `size_t` | in |

Return value: `HRESULT`.

Example:

```c
PCSTR const argv[] = { "/bin/sh", "-c", "echo ready" };
HRESULT hr = WslcSetProcessSettingsCmdLine(&processSettings, argv, _countof(argv));
```
