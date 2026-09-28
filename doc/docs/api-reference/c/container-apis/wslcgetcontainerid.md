# WslcGetContainerID

```c
STDAPI WslcGetContainerID(_In_ WslcContainer container, _Out_writes_(WSLC_CONTAINER_ID_BUFFER_SIZE) CHAR containerID[WSLC_CONTAINER_ID_BUFFER_SIZE]);
```

| Parameter | Type | Direction |
|---|---|---|
| `container` | `WslcContainer` | in |
| `containerID` | `CHAR[WSLC_CONTAINER_ID_BUFFER_SIZE]` | out |

Return value: `HRESULT`.

Example:

```c
CHAR containerID[WSLC_CONTAINER_ID_BUFFER_SIZE] = { 0 };
HRESULT hr = WslcGetContainerID(container, containerID);
```
