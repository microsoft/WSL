# WslcSetSessionSettingsHostLoopback

```c
STDAPI WslcSetSessionSettingsHostLoopback(_In_ WslcSessionSettings* sessionSettings, _In_opt_z_ PCSTR hostName);
```

| Parameter | Type | Direction |
|---|---|---|
| `sessionSettings` | `WslcSessionSettings*` | in |
| `hostName` | `PCSTR` | in, optional |

Return value: `HRESULT`.

Allows containers in the session to reach services listening on the Windows host loopback interface (for example, `localhost`) through `hostName`.

Host loopback is disabled by default. Passing `NULL` disables it. Services listening only on the Windows loopback interface may not expect connections from container workloads, so only enable this when needed.

Requires WSL 3.0.5 or later.

Containers must also have networking enabled to reach the host, for example with [`WslcSetContainerSettingsNetworkingMode`](../container-apis/wslcsetcontainersettingsnetworkingmode.md) and `WSLC_CONTAINER_NETWORKING_MODE_BRIDGED`.

Example:

```c
HRESULT hr = WslcSetSessionSettingsHostLoopback(&sessionSettings, "host.wslc.internal");
```
