# ProcessOutputMode

- `Discard` (default in `winrt_Process.h` and `winrt_ProcessSettings.h`)
- `Event` (`winrt_Process.cpp`)
- `Stream` (`winrt_Process.cpp`, `winrt_Container.cpp`)

Behavior:
- `Discard`: no stdout/stderr events or output streams.
- `Event`: stdout/stderr are delivered by callbacks and `OutputReceived` / `ErrorReceived`.
- `Stream`: `GetOutputStream(...)` can be used.

```cpp
procSettings.OutputMode(ProcessOutputMode::Event);
```
