# ImageProgressStatus

`ImageProgress` casts directly from `WslcImageProgressStatus`.

Underlying C values:

- `Unknown = 0`
- `Pulling = 1`
- `Waiting = 2`
- `Downloading = 3`
- `Verifying = 4`
- `Extracting = 5`
- `Complete = 6`

```cpp
auto status = progress.Status();
if (status == static_cast<ImageProgressStatus>(6))
{
    // complete
}
```
