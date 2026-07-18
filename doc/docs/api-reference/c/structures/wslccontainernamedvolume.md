# WslcContainerNamedVolume

```c
typedef struct WslcContainerNamedVolume
{
    _In_z_ PCSTR name;          // Name of the session volume (from WslcVhdRequirements.name)
    _In_z_ PCSTR containerPath; // Absolute path inside the container
    _In_ BOOL readOnly;
} WslcContainerNamedVolume;
```

| Field | Type |
|---|---|
| `name` | `PCSTR` |
| `containerPath` | `PCSTR` |
| `readOnly` | `BOOL` |
