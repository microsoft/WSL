# ProcessState

`Process::State()` casts directly from `WslcProcessState`.

Underlying C values:

- `Unknown = 0`
- `Running = 1`
- `Exited = 2`
- `Signalled = 3`

```cpp
if (process.State() == static_cast<ProcessState>(1))
{
    // running
}
```
