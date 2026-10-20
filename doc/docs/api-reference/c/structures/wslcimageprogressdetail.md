# WslcImageProgressDetail

```c
typedef struct WslcImageProgressDetail
{
    _Out_ uint64_t currentBytes; // bytes downloaded so far
    _Out_ uint64_t totalBytes;   // total bytes expected
} WslcImageProgressDetail;
```

| Field | Type |
|---|---|
| `currentBytes` | `uint64_t` |
| `totalBytes` | `uint64_t` |
