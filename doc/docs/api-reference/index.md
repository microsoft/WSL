# WSL container API developer reference

The WSL container API lets Windows app developers use Linux containers as part of their app logic. For an overview of the WSL container feature and the `wslc.exe` command-line interface, see [WSL container](https://learn.microsoft.com/windows/wsl/).

## API reference

The API is available across the following language projections. Each reference documents the same underlying capabilities, layered as **Session → Container → Process**.

| Language | Namespace / header | Reference |
|---|---|---|
| C | `wslcsdk.h` (`wslcsdk.lib` / `wslcsdk.dll`) | [C API reference](c/index.md) |
| C# | `Microsoft.WSL.Containers` | [C# API reference](csharp/index.md) |
| C++ | `Microsoft::WSL::Containers` | [C++ API reference](cpp/index.md) |

## Related content

- [WSL container](https://learn.microsoft.com/windows/wsl/wsl-container)
- [WSL docs](https://learn.microsoft.com/windows/wsl/)
