# ProcessOutputHandle

`Process::GetOutputStream(ProcessOutputHandle)` casts directly to `WslcProcessIOHandle`.
The underlying C handles are:
- `0` stdin
- `1` stdout
- `2` stderr



```cpp
auto stdoutStream = process.GetOutputStream(static_cast<ProcessOutputHandle>(1));
```
