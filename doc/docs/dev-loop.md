# Building WSL

## Prerequisites 

The following tools are required to build WSL: 

- CMake >= 3.25
    - Can be installed with `winget install Kitware.CMake`
- Visual Studio with the following components:
    - Windows SDK 26100
    - MSBuild
    - Universal Windows platform support for v143 build tools (X64 and ARM64)
    - MSVC v143 - VS 2022 C++ ARM64 build tools (Latest + Spectre) (X64 and ARM64)
    - C++ core features
    - C++ ATL for latest v143 tools (X64 and ARM64)
    - C++ Clang compiler for Windows
    - .NET desktop development
    - .NET WinUI app development tools

- Building WSL requires support for symbolic links. To ensure this capability, enable [Developer Mode](https://learn.microsoft.com/en-us/windows/apps/get-started/enable-your-device-for-development) in Windows Settings or execute the build process with Administrator privileges.

### ARM64 development

When building on ARM64 Windows, the [WiX](https://wixtoolset.org/) toolset (`wix.exe`) requires the **x64 .NET 6.0 runtime** because it is an x64 binary. The ARM64 .NET runtime alone is not sufficient.

To install the x64 .NET 6.0 runtime, run the following commands in PowerShell:

```powershell
# Download the official dotnet-install script
Invoke-WebRequest -Uri "https://dot.net/v1/dotnet-install.ps1" -OutFile "$env:TEMP\dotnet-install.ps1"

# Install the x64 .NET 6.0 runtime
powershell -ExecutionPolicy Bypass -File "$env:TEMP\dotnet-install.ps1" -Channel 6.0 -Runtime dotnet -Architecture x64 -InstallDir "C:\Program Files\dotnet\x64"
```

Then set the `DOTNET_ROOT_X64` environment variable so the runtime is discoverable:

```powershell
# Set for the current session
$env:DOTNET_ROOT_X64 = "C:\Program Files\dotnet\x64"

# Set permanently for your user
[System.Environment]::SetEnvironmentVariable("DOTNET_ROOT_X64", "C:\Program Files\dotnet\x64", "User")
```

> **Note:** You may need to restart VS Code or open a new terminal for the environment variable to take effect.

## Building WSL

Once you have cloned the repository, generate the Visual Studio solution by running:

```
cmake .
```

This will generate a `wsl.sln` file that you can build either with Visual Studio, or via `cmake --build .`.

Build parameters:

- `cmake . -A arm64`: Build a package for ARM64
- `cmake . -DCMAKE_BUILD_TYPE=Release`: Build for release
- `cmake . -DBUILD_BUNDLE=TRUE`: Build a bundle msix package (requires building ARM64 first)

Note: To build and deploy faster during development, see options in `UserConfig.cmake`.


## Deploying WSL 

Once the build is complete, you can install WSL by installing the MSI package found under `bin\<platform>\<target>\wsl.msi`, or by running `powershell tools\deploy\deploy-to-host.ps1`.

To deploy on a Hyper-V virtual machine, you can use `powershell tools\deploy\deploy-to-vm.ps1 -VmName <vm> -Username <username> -Password <password>`

## Running tests

To run unit tests, run: `bin\<platform>\<target>\test.bat`. There's quite a lot of tests so you probably don't want to run everything. Here's a reasonable subset:
`bin\<platform>\<target>\test.bat /name:*UnitTest*`

To run a specific test case run:
`bin\<platform>\<target>\test.bat /name:<class>::<test>`
Example: `bin\x64\debug\test.bat /name:UnitTests::UnitTests::ModernInstall` 

To run the tests for WSL1, add `-Version 1`. 
Example: `bin\x64\debug\test.bat -Version 1` 


After running the tests once, you can add `-f` to skip the package installation, which makes the tests faster (this requires test_distro to be the default WSL distribution).

Example:

```
wsl --set-default test_distro
bin\x64\debug\test.bat /name:*UnitTest* -f
```

## Debugging tests

See [debugging](debugging.md) for general debugging instructions.

To attach a debugger to the unit test process, use: `/waitfordebugger` when calling `test.bat`. 
Use `/breakonfailure` to automatically break on the first test failure. 

## Tips and tricks

**Building and deploying faster** 

To iterate faster, create a copy of [```UserConfig.cmake.sample```](https://github.com/microsoft/WSL/blob/master/UserConfig.cmake.sample):

```
copy UserConfig.cmake.sample UserConfig.cmake
```

And uncomment this line:

```
# set(WSL_DEV_BINARY_PATH "C:/wsldev")
```

This will change the build logic to build a smaller package that installs faster.
Also see:

- `WSL_BUILD_THIN_PACKAGE` to build an even smaller package
- `WSL_POST_BUILD_COMMAND` to automatically deploy the package during build

**Code formatting**

Every pull request needs to be clang-formatted before it can be merged.

The code can be manually formatted by running: `powershell .\FormatSource.ps1 -ModifiedOnly $false`.
To automatically check formatting when creating a commit, run: `tools\SetupClangFormat.bat`
