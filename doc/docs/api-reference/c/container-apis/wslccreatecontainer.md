# WslcCreateContainer

```c
STDAPI WslcCreateContainer(_In_ WslcSession session, _In_ const WslcContainerSettings* containerSettings, _Out_ WslcContainer* container, _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `containerSettings` | `const WslcContainerSettings*` | in |
| `container` | `WslcContainer*` | out |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

Example:

```c
WslcContainer container = NULL;
HRESULT hr = WslcCreateContainer(session, &containerSettings, &container, NULL);
```
