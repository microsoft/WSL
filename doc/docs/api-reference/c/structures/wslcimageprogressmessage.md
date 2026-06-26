# WslcImageProgressMessage

```c
typedef struct WslcImageProgressMessage
{
    _Out_ PCSTR id;                       // layer ID or digest
    _Out_ WslcImageProgressStatus status; // "Downloading", "Extracting", etc.
    _Out_ WslcImageProgressDetail detail;
} WslcImageProgressMessage;
```

| Field | Type |
|---|---|
| `id` | `PCSTR` |
| `status` | `WslcImageProgressStatus` |
| `detail` | `WslcImageProgressDetail` |
