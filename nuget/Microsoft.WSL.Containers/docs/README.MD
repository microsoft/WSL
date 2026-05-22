# Microsoft.WSL.Containers

> **⚠️ Preview:** This SDK is currently in preview and is subject to breaking changes
> in future releases without prior notice. Do not rely on API stability for production
> workloads.

The WSL Containers SDK provides APIs for creating and managing Linux containers running inside WSL, and MSBuild/CMake integration for building container images as part of your project.

**Supported platforms:** x64 and ARM64.

---

## Using the native C/C++ API

MSBuild (via NuGet) and CMake both configure include directories and link libraries automatically when you reference the package. Include the header and link:

```cpp
#include <wslcsdk.h>
```

**MSBuild:** No manual configuration needed — `wslcsdk.lib` and the include directory are wired up by the imported targets.

**CMake:**
```cmake
find_package(Microsoft.WSL.Containers REQUIRED)
target_link_libraries(my_app PRIVATE Microsoft.WSL.Containers::SDK)
```

The runtime DLL (`wslcsdk.dll`) is automatically copied to the output directory.

---

## Using the WinRT projection

### C++/WinRT

The package includes a WinRT metadata file and automatically:
- Adds `Microsoft.WSL.Containers.winmd` as a reference so C++/WinRT generates projection headers
- Injects an activation manifest into your binary so `RoGetActivationFactory` resolves the classes to `wslcsdk.dll` without COM registration

**MSBuild — C++/WinRT (requires the [Microsoft.Windows.CppWinRT](https://www.nuget.org/packages/Microsoft.Windows.CppWinRT) NuGet package):**
```cpp
#include <winrt/Microsoft.WSL.Containers.h>

auto settings = winrt::Microsoft::WSL::Containers::SessionSettings(L"my-session", L"C:\\path\\to\\storage");
```

To disable C++/WinRT integration (suppress the winmd reference and manifest injection):
```xml
<PropertyGroup>
  <WslcEnableCppWinRT>false</WslcEnableCppWinRT>
</PropertyGroup>
```

### C#/WinRT

**MSBuild — C# (.NET 8+):** The `wslcsdkcs.dll` projection assembly is referenced automatically.
```csharp
using Microsoft.WSL.Containers;

var settings = new SessionSettings("my-session", @"C:\path\to\storage");
```

---

## Building container images

The package integrates the `wslc` CLI into your build system so container images are built and saved as part of your normal project build, with incremental rebuild support (images only rebuild when sources change).

### MSBuild

Add `WslcImage` items to your project. Each item builds an image and saves it to a `.tar` archive at build time:

```xml
<ItemGroup>
  <WslcImage Include="my-server"
             Image="ghcr.io/myorg/my-server:latest"
             Dockerfile="container/Dockerfile"
             Context="container/"
             Sources="container/src/**"
             TarLocation="$(OutDir)my-server.tar" />
</ItemGroup>
```

| Metadata | Required | Description |
|---|---|---|
| `Image` | Yes | Container image reference (`:latest` appended if no tag) |
| `Dockerfile` | Yes | Path to the Dockerfile |
| `Context` | Yes | Build context directory |
| `Sources` | No | Glob patterns for incremental rebuild tracking (defaults to all files in `Context`) |
| `TarLocation` | No | Output path for the saved `.tar` (defaults to `$(OutDir)<name>.tar`) |

**Optional MSBuild properties:**

| Property | Default | Description |
|---|---|---|
| `WslcCliPath` | `wslc` | Path to the `wslc` CLI executable |
| `WslcPruneAfterBuild` | `false` | Run `wslc image prune` after each successful build |
| `WslcTreatPruneFailureAsError` | `false` | Fail the build if the post-build prune fails |
| `WslcPlatform` | `$(Platform)` | Override the detected platform (x64 or arm64) |

### CMake

```cmake
find_package(Microsoft.WSL.Containers REQUIRED)

wslc_add_image(my-server
    IMAGE        ghcr.io/myorg/my-server:latest
    DOCKERFILE   container/Dockerfile
    CONTEXT      container/
    SOURCES      container/src/*.cpp container/src/*.h
    TAR_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/my-server.tar
)

add_dependencies(my_app my-server)
```

Set `WSLC_CLI_PATH` to override the `wslc` executable location. Pass `PRUNE_AFTER_BUILD` to prune dangling images after each build.

---

## Prerequisites

- **WSL** — Install with `wsl --install --no-distribution` (provides the `wslc` CLI)
- **C++/WinRT** — Install the [Microsoft.Windows.CppWinRT](https://www.nuget.org/packages/Microsoft.Windows.CppWinRT) NuGet package for C++/WinRT projection support
