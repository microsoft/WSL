# WslcContainerVolume

```c
typedef struct WslcContainerVolume
{
    _In_z_ PCWSTR windowsPath;
    _In_z_ PCSTR containerPath;
    _In_ BOOL readOnly;
} WslcContainerVolume;
```

| Field | Type |
|---|---|
| `windowsPath` | `PCWSTR` |
| `containerPath` | `PCSTR` |
| `readOnly` | `BOOL` |
