# SessionTerminationHandler

Observed use:

- `Session::Terminated` raises one argument: `SessionTerminationReason reason`.

```cpp
session.Terminated([](SessionTerminationReason reason)
{
    printf("terminated: %d\n", static_cast<int>(reason));
});
```
