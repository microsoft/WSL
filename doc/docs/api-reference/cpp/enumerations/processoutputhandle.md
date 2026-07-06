# ProcessOutputHandle

`Process::GetOutputStream(ProcessOutputHandle)` accepts these values:

- `StandardOutput = 1`
- `StandardError = 2`

```cpp
auto stdoutStream = process.GetOutputStream(ProcessOutputHandle::StandardOutput);
```
