# WslcRegisterSessionCrashDumpCallback

```c
STDAPI WslcRegisterSessionCrashDumpCallback(
    _In_ WslcSession session,
    _In_ WslcSessionCrashDumpCallback crashDumpCallback,
    _In_opt_ PVOID crashDumpContext,
    _Out_ WslcCrashDumpSubscription* subscription,
    _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `crashDumpCallback` | `WslcSessionCrashDumpCallback` | in |
| `crashDumpContext` | `PVOID` | in, optional |
| `subscription` | `WslcCrashDumpSubscription*` | out |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Example:

```c
void CALLBACK OnCrashDump(const WslcSessionCrashDumpInfo* info, PVOID context)
{
    UNREFERENCED_PARAMETER(context);
    wprintf(L"dump=%ls\n", info->dumpPath);
}

WslcCrashDumpSubscription subscription = NULL;
HRESULT hr = WslcRegisterSessionCrashDumpCallback(
    session,
    OnCrashDump,
    NULL,
    &subscription,
    NULL);
```
