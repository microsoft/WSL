# ProcessOutputMode

Underlying values:

- `Discard = 0`
- `Stream = 1`
- `Event = 2`

Behavior:

- `Discard`: no stdout/stderr events or output streams.
- `Stream`: `GetOutputStream(...)` can be used.
- `Event`: stdout/stderr are delivered by callbacks and `OutputReceived` / `ErrorReceived`.

```cpp
procSettings.OutputMode(ProcessOutputMode::Event);
```
