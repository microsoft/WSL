# WslcActivateContainerIOCallbacks

Starts delivering the init process I/O callbacks of a container that was started with
`WSLC_CONTAINER_START_FLAG_DEFER_IO_CALLBACKS`.

```c
STDAPI WslcActivateContainerIOCallbacks(_In_ WslcContainer container);
```

| Parameter | Type | Direction |
|---|---|---|
| `container` | `WslcContainer` | in |

Return value: `HRESULT`.

Deferring delivery lets the caller finish setting up callback state that depends on the started
container, such as the handle returned by [`WslcGetContainerInitProcess`](wslcgetcontainerinitprocess.md),
before any callback runs.

Until this function is called, process output is held in the I/O pipes. If the pipes fill up, the init
process blocks on writes. Callbacks that are never activated are discarded when the container is released,
and `onExit` is not called.

The function succeeds without effect if the container has no init process I/O callbacks, or if they are
already active.

```c
HRESULT hr = WslcSetContainerInitProcessIOCallbacks(container, &callbacks, context);
if (SUCCEEDED(hr))
{
    hr = WslcStartContainer(
        container, WSLC_CONTAINER_START_FLAG_ATTACH | WSLC_CONTAINER_START_FLAG_DEFER_IO_CALLBACKS, NULL);
}

if (SUCCEEDED(hr))
{
    hr = WslcGetContainerInitProcess(container, &context->initProcess);
}

if (SUCCEEDED(hr))
{
    hr = WslcActivateContainerIOCallbacks(container);
}
```
