# WslcSessionCrashDumpInfo

```c
typedef struct WslcSessionCrashDumpInfo
{
    _Field_z_ PCWSTR dumpPath;
    _Field_z_ PCSTR processName;
    uint32_t pid;
    uint32_t signal;
    uint64_t timestamp;
} WslcSessionCrashDumpInfo;
```

| Field | Type |
|---|---|
| `dumpPath` | `PCWSTR` |
| `processName` | `PCSTR` |
| `pid` | `uint32_t` |
| `signal` | `uint32_t` |
| `timestamp` | `uint64_t` |
