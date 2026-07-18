# WslcGetSessionTerminationEvent

```c
STDAPI WslcGetSessionTerminationEvent(_In_ WslcSession session, _Out_ HANDLE* terminationEvent);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `terminationEvent` | `HANDLE*` | out |

Return value: `HRESULT`.

Example:

```c
HANDLE terminationEvent = NULL;
HRESULT hr = WslcGetSessionTerminationEvent(session, &terminationEvent);
if (SUCCEEDED(hr))
{
    WaitForSingleObject(terminationEvent, 1000);
}
```
