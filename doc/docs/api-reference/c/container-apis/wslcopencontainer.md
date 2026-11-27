# WslcOpenContainer

Opens an existing container in a session by name, full ID, or an unambiguous partial ID prefix.

```c
STDAPI WslcOpenContainer(
    _In_ WslcSession session,
    _In_z_ PCSTR nameOrId,
    _Out_ WslcContainer* container,
    _Outptr_opt_result_z_ PWSTR* errorMessage);
```

| Parameter | Type | Direction |
|---|---|---|
| `session` | `WslcSession` | in |
| `nameOrId` | `PCSTR` | in |
| `container` | `WslcContainer*` | out |
| `errorMessage` | `PWSTR*` | out, optional |

Return value: `HRESULT`.

The caller owns the returned handle and must release it with [`WslcReleaseContainer`](wslcreleasecontainer.md).

The function returns `WSLC_E_CONTAINER_NOT_FOUND` if no container matches, or
`WSLC_E_CONTAINER_PREFIX_AMBIGUOUS` if a partial ID matches more than one container.

```c
WslcContainer container = NULL;
HRESULT hr = WslcOpenContainer(session, "web", &container, NULL);
if (SUCCEEDED(hr))
{
    // Use the container.
    WslcReleaseContainer(container);
}
```
