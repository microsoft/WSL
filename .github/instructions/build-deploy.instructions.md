---
description: 'WSL build system, CMake configuration, ARM64, UserConfig, and deployment'
applyTo: '**/CMakeLists.txt, **/*.cmake, tools/deploy/**'
---

# Build & Deploy — WSL

Reference: https://wsl.dev/dev-loop/

## Prerequisites

- CMake >= 3.25 (`winget install Kitware.CMake`)
- Visual Studio with: Windows SDK 26100, MSBuild, UWP v143 build tools (X64+ARM64), MSVC v143 ARM64 (Latest+Spectre), C++ core features, C++ ATL v143, C++ Clang, .NET desktop dev, .NET WinUI app dev tools
- Developer Mode enabled OR Administrator privileges (for symlinks)
- **Full builds ONLY work on Windows**

### ARM64-Specific

WiX (`wix.exe`) requires the **x64 .NET 6.0 runtime** on ARM64 Windows:
```powershell
Invoke-WebRequest -Uri "https://dot.net/v1/dotnet-install.ps1" -OutFile "$env:TEMP\dotnet-install.ps1"
powershell -ExecutionPolicy Bypass -File "$env:TEMP\dotnet-install.ps1" -Channel 6.0 -Runtime dotnet -Architecture x64 -InstallDir "C:\Program Files\dotnet\x64"
$env:DOTNET_ROOT_X64 = "C:\Program Files\dotnet\x64"
[System.Environment]::SetEnvironmentVariable("DOTNET_ROOT_X64", "C:\Program Files\dotnet\x64", "User")
```

## Building

```powershell
cmake .                  # Generate Visual Studio solution (wsl.sln)
cmake --build . -- -m    # Build (20–45 min, NEVER CANCEL)
```

| Parameter | Purpose |
|-----------|---------|
| `cmake . -A arm64` | Build for ARM64 |
| `cmake . -DCMAKE_BUILD_TYPE=Release` | Release build |
| `cmake . -DBUILD_BUNDLE=TRUE` | Bundle MSIX (requires ARM64 built first) |

## Faster Development Iteration

Copy `UserConfig.cmake.sample` → `UserConfig.cmake` and uncomment:
```cmake
set(WSL_DEV_BINARY_PATH "C:/wsldev")
```

| Option | Purpose |
|--------|---------|
| `WSL_DEV_BINARY_PATH` | Smaller package, faster install |
| `WSL_BUILD_THIN_PACKAGE` | Even smaller package |
| `WSL_POST_BUILD_COMMAND` | Auto-deploy during build |

## Deploying

| Method | Command |
|--------|---------|
| MSI install | `bin\<platform>\<target>\wsl.msi` |
| Deploy script | `powershell tools\deploy\deploy-to-host.ps1` |
| Hyper-V VM | `powershell tools\deploy\deploy-to-vm.ps1 -VmName <vm> -Username <user> -Password <pass>` |

## Timeouts

| Operation | Duration | Set Timeout |
|-----------|----------|-------------|
| Full build | 20–45 min | 60+ min |

> **NEVER cancel a build in progress.**
