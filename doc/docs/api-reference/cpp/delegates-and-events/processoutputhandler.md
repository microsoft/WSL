# ProcessOutputHandler

Observed use:

- `Process::OutputReceived` and `Process::ErrorReceived` raise one argument containing raw output bytes.
- The wrapper forwards a `winrt::array_view<const uint8_t>` produced from the C callback buffer.

```cpp
process.OutputReceived([](auto const& data)
{
    std::string text(data.begin(), data.end());
    printf("stdout: %s\n", text.c_str());
});
```
