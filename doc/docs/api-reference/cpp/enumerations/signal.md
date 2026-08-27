# Signal

`Container::Stop()` and `Process::Signal()` cast directly to `WslcSignal`.

Underlying values:

- `None = 0`
- `SIGHUP = 1`
- `SIGINT = 2`
- `SIGQUIT = 3`
- `SIGKILL = 9`
- `SIGTERM = 15`

```cpp
process.Signal(Signal::SIGINT);
container.Stop(Signal::SIGTERM, std::chrono::seconds(10));
```
