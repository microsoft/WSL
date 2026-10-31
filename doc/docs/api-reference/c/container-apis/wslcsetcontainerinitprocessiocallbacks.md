# WslcSetContainerInitProcessIOCallbacks

Sets callbacks that receive the standard output, standard error, and exit code from the init process
of a container that has not been started. See
[`WslcProcessCallbacks`](../structures/wslcprocesscallbacks.md) for the individual callbacks.

```c
STDAPI WslcSetContainerInitProcessIOCallbacks(
    _In_ WslcContainer container,
    _In_ const WslcProcessCallbacks* callbacks,
    _In_opt_ PVOID context);
```

| Parameter | Type | Direction |
|---|---|---|
| `container` | `WslcContainer` | in |
| `callbacks` | [`const WslcProcessCallbacks*`](../structures/wslcprocesscallbacks.md) | in |
| `context` | `PVOID` | in, optional |

Return value: `HRESULT`.

Call this function before [`WslcStartContainer`](wslcstartcontainer.md), and start the container with
`WSLC_CONTAINER_START_FLAG_ATTACH`. It has no effect after the container is already running.

Registering any callback through this function consumes both process I/O handles, so neither can
subsequently be acquired with `WslcGetProcessIOHandle`. If registered, `onExit` runs after buffered
I/O has been delivered and the I/O callbacks have returned. The caller owns `context` and must keep
it valid until all registered callbacks have returned.

```c
WslcProcessCallbacks callbacks = {0};
callbacks.onStdOut = OnStdIO;
callbacks.onStdErr = OnStdIO;
callbacks.onExit = OnProcessExit;

HRESULT hr = WslcSetContainerInitProcessIOCallbacks(container, &callbacks, context);
if (SUCCEEDED(hr))
{
    hr = WslcStartContainer(container, WSLC_CONTAINER_START_FLAG_ATTACH, NULL);
}
```
