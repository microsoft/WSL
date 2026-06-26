# WslcTagImageOptions

```c
typedef struct WslcTagImageOptions
{
    _In_z_ PCSTR image; // Source image name or ID.
    _In_z_ PCSTR repo;  // Target repository name.
    _In_z_ PCSTR tag;   // Target tag name.
} WslcTagImageOptions;
```

| Field | Type |
|---|---|
| `image` | `PCSTR` |
| `repo` | `PCSTR` |
| `tag` | `PCSTR` |
