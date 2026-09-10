# WSL container API developer reference

The WSL container API lets Windows app developers use Linux containers as part of their app logic. For an overview of the WSL container feature and the `wslc.exe` command-line interface, see [WSL container](https://learn.microsoft.com/windows/wsl/).

!!! important

    The WSL container API is currently in **preview** and may have breaking changes in future releases. Please use this preview to evaluate feasibility and then only deploy production grade code once this API goes to general availability in fall 2026.


## API reference

The API is available across the following language projections. Each reference documents the same underlying capabilities, layered as **Session → Container → Process**.

| Language | Namespace / header | Reference |
|---|---|---|
| C | `wslcsdk.h` (`wslcsdk.lib` / `wslcsdk.dll`) | [C API reference](c/index.md) |
| C# | `Microsoft.WSL.Containers` | [C# API reference](csharp/index.md) |
| C++ | `Microsoft::WSL::Containers` | [C++ API reference](cpp/index.md) |

## Related content

- [WSL container](https://learn.microsoft.com/windows/wsl/)
- [Get started with Linux containers](https://learn.microsoft.com/windows/wsl/)
