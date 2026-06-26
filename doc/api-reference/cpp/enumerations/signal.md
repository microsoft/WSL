# Signal

`Container::Stop()` and `Process::Signal()` cast directly to `WslcSignal`.

Underlying C values in `wslcsdk.h`:
- `0` none
- `1` SIGHUP
- `2` SIGINT
- `3` SIGQUIT
- `9` SIGKILL
- `15` SIGTERM



```cpp
process.Signal(static_cast<Signal>(2));   // SIGINT
container.Stop(static_cast<Signal>(15), std::chrono::seconds(10)); // SIGTERM
```
