# SessionTerminationReason

`Session::OnTerminated` converts `WslcSessionTerminationReason` directly to the WinRT enum.

Underlying C values:

- `Unknown = 0`
- `Shutdown = 1`
- `Crashed = 2`

```cpp
session.Terminated([](SessionTerminationReason reason)
{
    if (reason == static_cast<SessionTerminationReason>(2))
    {
        // crashed
    }
});
```
