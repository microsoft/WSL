# WslcStdIOCallback

```c
typedef __callback void(CALLBACK* WslcStdIOCallback)(
    WslcProcessIOHandle ioHandle, _In_reads_bytes_(dataBytes) const BYTE* data, _In_ uint32_t dataBytes, _In_opt_ PVOID context);
```

Header notes:

- Only `STDOUT` and `STDERR` receive callbacks.
- `data` is owned by WSLC and is only valid during the callback.
- The buffer is not null-terminated.
- The callback should return promptly.
