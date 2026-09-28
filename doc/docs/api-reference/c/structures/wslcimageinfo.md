# WslcImageInfo

```c
typedef struct WslcImageInfo
{
    CHAR name[WSLC_IMAGE_NAME_LENGTH];
    uint8_t sha256[32];
    int64_t sizeBytes;
    uint64_t createdUnixTime;
} WslcImageInfo;
```

| Field | Type |
|---|---|
| `name` | `CHAR[WSLC_IMAGE_NAME_LENGTH]` |
| `sha256` | `uint8_t[32]` |
| `sizeBytes` | `int64_t` |
| `createdUnixTime` | `uint64_t` |
