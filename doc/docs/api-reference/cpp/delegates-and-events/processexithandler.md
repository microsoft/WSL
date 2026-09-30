# ProcessExitHandler

Observed use:

- `Process::Exited` raises one `int32_t exitCode`.

```cpp
process.Exited([](int32_t exitCode)
{
    printf("exit code: %d\n", exitCode);
});
```

---
