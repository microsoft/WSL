# WslcSetProcessSettingsCallbacks

```c
STDAPI WslcSetProcessSettingsCallbacks(_In_ WslcProcessSettings* processSettings, _In_ const WslcProcessCallbacks* callbacks, _In_opt_ PVOID context);
```

| Parameter | Type | Direction |
|---|---|---|
| `processSettings` | `WslcProcessSettings*` | in |
| `callbacks` | `const WslcProcessCallbacks*` | in |
| `context` | `PVOID` | in, optional |

Return value: `HRESULT`.

Header note: using callbacks consumes the process I/O handles and prevents later acquisition through `WslcGetProcessIOHandle`.

Example:

```c
void CALLBACK OnStdOut(WslcProcessIOHandle ioHandle, const BYTE* data, uint32_t dataBytes, PVOID context)
{
    UNREFERENCED_PARAMETER(ioHandle);
    UNREFERENCED_PARAMETER(context);
    fwrite(data, 1, dataBytes, stdout);
}

void CALLBACK OnExit(INT32 exitCode, PVOID context)
{
    UNREFERENCED_PARAMETER(context);
    printf("exit=%ld\n", (long)exitCode);
}

WslcProcessCallbacks callbacks = { 0 };
callbacks.onStdOut = OnStdOut;
callbacks.onStdErr = OnStdOut;
callbacks.onExit = OnExit;

HRESULT hr = WslcSetProcessSettingsCallbacks(&processSettings, &callbacks, NULL);
```
