# Windows Subsystem for Linux (WSL)

**ALWAYS reference these instructions first and fallback to search or bash commands only when you encounter unexpected information that does not match the info here.**

WSL is the Windows Subsystem for Linux - a compatibility layer for running Linux binary executables natively on Windows. This repository contains the core Windows components that enable WSL functionality.

## Coding Conventions

### Naming

- **Classes/Structs**: `PascalCase` (e.g., `ConsoleProgressBar`, `DeviceHostProxy`)
- **Functions/Methods**: `PascalCase()` (e.g., `GetFamilyName()`, `MultiByteToWide()`)
- **Member variables**: `m_camelCase` (e.g., `m_isOutputConsole`, `m_outputHandle`)
- **Local variables**: `camelCase` (e.g., `distroGuidString`, `asyncResponse`)
- **Constants**: `c_camelCase` with `constexpr` (e.g., `constexpr size_t c_progressBarWidth = 58;`)
- **Namespaces**: lowercase with `::` nesting (e.g., `wsl::windows::common::registry`)
- **Enums**: `PascalCaseValue` (e.g., `LxssDistributionStateInstalled`)
- **Windows types**: Keep as-is (`LPCWSTR`, `HRESULT`, `DWORD`, `ULONG`, `GUID`)

### Error Handling

Use WIL (Windows Implementation Libraries) macros — **never** bare `if (FAILED(hr))`:
- `THROW_IF_FAILED(hr)` — throw on HRESULT failure
- `THROW_HR_IF(hr, condition)` — conditional throw
- `THROW_HR_IF_MSG(hr, condition, fmt, ...)` — conditional throw with message
- `THROW_IF_NULL_ALLOC(ptr)` — throw on null allocation
- `THROW_LAST_ERROR_IF(condition)` — throw last Win32 error
- `RETURN_IF_FAILED(hr)` — return HRESULT on failure (no throw)
- `RETURN_LAST_ERROR_IF_EXPECTED(condition)` — expected failure path
- `LOG_IF_FAILED(hr)` — log but don't throw
- `CATCH_LOG()` — catch and log exceptions

For user-facing errors, set a localized message before throwing:
```cpp
THROW_HR_WITH_USER_ERROR(E_INVALIDARG, Localization::MessageConfigInvalidBoolean(name, value));
```

At API boundaries (COM interfaces), return `HRESULT` with out params. Internal code throws exceptions.

### Memory Management and RAII

Use WIL smart pointers — **never** raw `CloseHandle()` or manual cleanup:
- `wil::unique_handle` — kernel handles
- `wil::com_ptr<T>` — COM objects
- `wil::unique_hfile` — file handles
- `wil::unique_hkey` — registry keys
- `wil::unique_event` — events
- `wil::unique_hlocal_string` — HLOCAL strings
- `wil::unique_cotaskmem_string` — CoTaskMem strings

For non-standard resource types, use `wil::unique_any<Type, Deleter, Fn>`.

For cleanup scopes, use `wil::scope_exit`:
```cpp
auto cleanup = wil::scope_exit([&] { registry::DeleteKey(LxssKey, guid.c_str()); });
// ... work ...
cleanup.release(); // dismiss on success
```

### Synchronization

Use `wil::srwlock` (Slim Reader/Writer locks) with SAL annotations:
```cpp
mutable wil::srwlock m_lock;
_Guarded_by_(m_lock) std::vector<Entry> m_entries;
```

### Strings

- `std::wstring` / `std::wstring_view` are dominant throughout the codebase
- Use `MultiByteToWide()` / `WideToMultiByte()` from `stringshared.h` for conversions
- Use `std::format()` for formatting (the repo defines `std::formatter<std::wstring, char>` for wide-to-narrow support)
- The `STRING_TO_WIDE_STRING()` macro handles compile-time conversion

### Copy/Move Semantics

Use macros from `defs.h` to declare copy/move behavior:
```cpp
NON_COPYABLE(MyClass);
NON_MOVABLE(MyClass);
DEFAULT_MOVABLE(MyClass);
```

### Headers

- Use `#pragma once` (no traditional `#ifndef` include guards)
- In Windows C++ components, every `.cpp` file must start with `#include "precomp.h"`
- Linux-side code (`src/linux/`) does not use precompiled headers
- Use `.h` for C-compatible headers, `.hpp` for C++-only headers
- Include order is enforced by `.clang-format` (precomp first, then system, then project)

### Copyright Headers

Use this single-line format for new files:
```cpp
// Copyright (C) Microsoft Corporation. All rights reserved.
```

Some older files use the block format (`/*++ Copyright (c) Microsoft. All rights reserved. ... --*/`). Match the surrounding files in the same directory when editing.

### Localization

- Use `wsl::shared::Localization::MessageXxx()` static methods for user-facing strings
- Use `EMIT_USER_WARNING(Localization::MessageXxx(...))` for non-fatal config warnings
- All new user-facing strings must have entries in `localization/strings/en-US/Resources.resw`
- In Resources.resw comments, use `{Locked="..."}` to prevent translation of `.wslconfig` property key names

### Telemetry and Logging

- `WSL_LOG(Name, ...)` — standard trace event
- `WSL_LOG_DEBUG(Name, ...)` — debug-only (compiled out in release via `if constexpr`)
- `WSL_LOG_TELEMETRY(Name, Tag, ...)` — metrics with privacy tag and version info
- Provider: `g_hTraceLoggingProvider`, initialized via `WslTraceLoggingInitialize()`

### Platform Conditionals

Prefer `constexpr` checks over `#ifdef` where possible:
```cpp
if constexpr (wsl::shared::Debug) { /* debug-only code */ }
if constexpr (wsl::shared::Arm64) { /* ARM64-specific code */ }
```

For compiler-specific code, use `#ifdef _MSC_VER` (Windows) / `#ifdef __GNUC__` (Linux).

### Formatting

Enforced by `.clang-format`:
- 130 character column limit
- 4-space indentation, no tabs
- Allman-style braces (opening brace on new line for classes, functions, structs, control statements)
- Left-aligned pointers (`int* ptr`, not `int *ptr`)
- `InsertBraces: true` — all control statements must have braces

### IDL / COM Conventions

When modifying service interfaces (`src/windows/service/inc/`):
- Interface attributes on separate lines: `[uuid(...), pointer_default(unique), object]`
- String params: `[in, unique] LPCWSTR` with `[string]` for marshaled strings
- Handle params: `[in, system_handle(sh_file)] HANDLE`
- User-facing errors: pass `[in, out] LXSS_ERROR_INFO* Error`
- **Adding methods to an existing interface is an ABI break** — create a new versioned interface with a new IID
- Custom error codes: `WSL_E_xxx` via `MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, WSL_E_BASE + N)`

### Config File (.wslconfig) Conventions

When adding settings to `src/shared/configfile/`:
- Format is `.gitconfig`-style INI: `[section]`, `key = value`, `#` comments, `\` line continuation
- Use the `ConfigKey` template class for type-safe parsing
- Supported types: `bool`, `int` (hex/octal), `std::string`, `std::wstring`, `MemoryString`, `MacAddress`, enum maps
- Report invalid values with `EMIT_USER_WARNING(Localization::MessageConfigXxx(...))`
- New settings require a corresponding localization string in Resources.resw

## Repository Navigation

### Key Directories
- `src/windows/` — Main Windows WSL service components
- `src/linux/` — Linux-side WSL components
- `src/shared/` — Shared code between Windows and Linux
- `test/windows/` — Windows-based tests (TAEF framework)
- `test/linux/unit_tests/` — Linux unit test suite
- `doc/` — Documentation source (MkDocs)
- `tools/` — Build and deployment scripts
- `distributions/` — Distribution validation and metadata
- `localization/` — Localized string resources

### Namespace → Directory Map

| Namespace | Location |
|---|---|
| `wsl::shared::` | `src/shared/` |
| `wsl::windows::common::` | `src/windows/common/` |
| `wsl::windows::service::` | `src/windows/service/exe/` |
| `wsl::core::` | `src/windows/service/exe/` |
| `wsl::core::networking::` | `src/windows/common/` + `src/windows/service/exe/` |
| `wsl::linux::` | `src/linux/init/` |

### Key Files
- `src/shared/inc/defs.h` — Shared platform definitions (NON_COPYABLE, Debug, Arm64, etc.)
- `src/shared/inc/stringshared.h` — String conversion utilities
- `src/windows/common/WslTelemetry.h` — Telemetry macros
- `src/windows/common/ExecutionContext.h` — Error context and user-facing error macros
- `src/windows/service/inc/wslservice.idl` — Main service COM interface definitions
- `src/windows/service/inc/wslc.idl` — Container COM interface definitions
- `src/windows/inc/WslPluginApi.h` — Plugin API header
- `src/shared/configfile/configfile.h` — Config file parser
- `.clang-format` — Code formatting rules (130 col, 4-space indent, Allman braces)

## Building and Deploying

### Critical Platform Requirements
- **Full builds ONLY work on Windows** with Visual Studio and Windows SDK 26100
- **DO NOT attempt to build the main WSL components on Linux** — they require Windows-specific APIs, MSBuild, and Visual Studio toolchain
- Many validation and development tasks CAN be performed on Linux (documentation, formatting, Python validation scripts)

### Windows Build Requirements
- CMake >= 3.25 (`winget install Kitware.CMake`)
- Visual Studio with these components:
  - Windows SDK 26100
  - MSBuild
  - Universal Windows platform support for v143 build tools (X64 and ARM64)
  - MSVC v143 - VS 2022 C++ ARM64 build tools (Latest + Spectre) (X64 and ARM64)
  - C++ core features
  - C++ ATL for latest v143 tools (X64 and ARM64)
  - C++ Clang compiler for Windows
  - .NET desktop development
  - .NET WinUI app development tools
- Enable Developer Mode in Windows Settings OR run with Administrator privileges (required for symbolic link support)

### Building WSL (Windows Only)
1. Clone the repository
2. Generate Visual Studio solution: `cmake .`
3. Build: `cmake --build . -- -m` OR open `wsl.sln` in Visual Studio

Build parameters:
- `cmake . -A arm64` — Build for ARM64
- `cmake . -DCMAKE_BUILD_TYPE=Release` — Release build
- `cmake . -DBUILD_BUNDLE=TRUE` — Build bundle MSIX package (requires ARM64 built first)

### Deploying WSL (Windows Only)
- Install MSI: `bin\<platform>\<target>\wsl.msi`
- OR use script: `powershell tools\deploy\deploy-to-host.ps1`
- For Hyper-V VM: `powershell tools\deploy\deploy-to-vm.ps1 -VmName <vm> -Username <user> -Password <pass>`

## Testing

### Writing Tests (TAEF Framework)

Tests use TAEF. See `.github/copilot/test.md` for detailed patterns and macros.

Key points:
- Use `WSL_TEST_CLASS(Name)` — not raw `BEGIN_TEST_CLASS`
- Use `VERIFY_*` macros for assertions (`VERIFY_ARE_EQUAL`, `VERIFY_IS_TRUE`, etc.)
- Skip macros: `WSL1_TEST_ONLY()`, `WSL2_TEST_ONLY()`, `SKIP_TEST_ARM64()`
- Test infrastructure is in `test/windows/Common.h`

### Running Tests (Windows Only)

**CRITICAL: ALWAYS build the ENTIRE project before running tests:**
```powershell
cmake --build . -- -m
bin\<platform>\<target>\test.bat
```

**Why full build is required:**
- Tests depend on multiple components (libwsl.dll, wsltests.dll, wslservice.exe, etc.)
- Partial builds will cause test failures
- **DO NOT skip the full build step even if only one file changed**

Test execution:
- Run all tests: `bin\<platform>\<target>\test.bat`
- Run subset: `bin\<platform>\<target>\test.bat /name:*UnitTest*`
- Run specific test: `bin\<platform>\<target>\test.bat /name:<class>::<test>`
- WSL1 tests: Add `-Version 1` flag
- Fast mode (after first run): Add `-f` flag (requires `wsl --set-default test_distro`)
- **Requires Administrator privileges**

Test debugging:
- Attach WinDbgX automatically: `/attachdebugger`
- Wait for debugger (manual attach): `/waitfordebugger`
- Break on failure: `/breakonfailure`
- Run in-process: `/inproc`

### Linux Unit Tests (Linux Only)
- Location: `test/linux/unit_tests/`
- Build script: `test/linux/unit_tests/build_tests.sh`
- **Note**: Requires specific Linux build environment setup not covered in main build process

## Cross-Platform Validation Tasks

### Documentation (Works on Linux/Windows)
- Install tools: `pip install mkdocs-mermaid2-plugin mkdocs --break-system-packages`
- Build docs: `mkdocs build -f doc/mkdocs.yml`
- Output location: `doc/site/`
- **Note**: May show warnings about mermaid CDN access on restricted networks

### Code Formatting and Validation
- Format all source (Windows, requires `cmake .` first): `.\FormatSource.ps1`
- Format check (Linux/cross-platform): `clang-format --dry-run --style=file <files>`
- Validate copyright headers: `python3 tools/devops/validate-copyright-headers.py`
  - **Note**: Will report missing headers in generated/dependency files (`_deps/`), which is expected
- Validate localization: `python3 tools/devops/validate-localization.py`
  - **Note**: Only works after Windows build (requires `localization/strings/en-US/Resources.resw`)

### Distribution Validation (Limited on Linux)
- Validate distribution info: `python3 distributions/validate.py distributions/DistributionInfo.json`
- **Note**: May fail on Linux due to network restrictions accessing distribution URLs

### Pre-commit Checklist
Always run before committing:
1. `.\FormatSource.ps1` to verify formatting on changed C++ files
2. `python3 tools/devops/validate-copyright-headers.py` (ignore `_deps/` warnings)
3. `mkdocs build -f doc/mkdocs.yml` if documentation changed
4. Full Windows build if core components changed

**Note**: The `.gitignore` properly excludes build artifacts (`*.sln`, `*.dll`, `*.pdb`, `obj/`, `bin/`, etc.) — do not commit these files.

## Frequently Used Commands

### Windows Development
```powershell
# Initial setup
cmake .
cmake --build . -- -m

# Deploy and test
powershell tools\deploy\deploy-to-host.ps1
wsl --version

# Run tests
bin\x64\debug\test.bat
```

### Cross-Platform Validation
```powershell
# Documentation
mkdocs build -f doc/mkdocs.yml

# Code formatting (Windows)
.\FormatSource.ps1

# Copyright header validation (reports expected issues in _deps/)
python3 tools/devops/validate-copyright-headers.py

# Distribution validation (may fail on networks without external access)
python3 distributions/validate.py distributions/DistributionInfo.json
```

## Debugging and Logging

### ETL Tracing (Windows Only)
```powershell
# Collect traces
wpr -start diagnostics\wsl.wprp -filemode
# [reproduce issue]
wpr -stop logs.ETL

# Available profiles:
# - WSL (default) - General WSL tracing
# - WSL-Storage - Enhanced storage tracing
# - WSL-Networking - Comprehensive networking tracing
# - WSL-HvSocket - HvSocket-specific tracing
# Example: wpr -start diagnostics\wsl.wprp!WSL -filemode
```

### Log Analysis Tools
- Use WPA (Windows Performance Analyzer) for ETL traces
- Key providers: `Microsoft.Windows.Lxss.Manager`, `Microsoft.Windows.Subsystem.Lxss`

### Debug Console (Linux)
Add to `%USERPROFILE%\.wslconfig`:
```ini
[wsl2]
debugConsole=true
```

### Common Debugging Commands
- Debug shell: `wsl --debug-shell`
- Collect WSL logs: `powershell diagnostics\collect-wsl-logs.ps1`
- Network logs: `powershell diagnostics\collect-wsl-logs.ps1 -LogProfile networking`

## Timing and Timeout Guidelines

**NEVER CANCEL these operations — always wait for completion:**

| Operation | Typical Duration | Minimum Timeout |
|---|---|---|
| Full Windows build | 20-45 minutes | 60+ minutes |
| Full test suite | 30-60 minutes | 90+ minutes |
| Unit test subset | 5-15 minutes | 30+ minutes |
| Documentation build | ~0.5 seconds | 5+ minutes |
| Distribution validation | 2-5 minutes | 15+ minutes |

## CI/CD Integration

### GitHub Actions
- **distributions.yml** — Validates distribution metadata (Linux)
- **documentation.yml** — Builds and deploys docs (Linux)
- **modern-distributions.yml** — Tests modern distribution support

## Development Environment Setup

### Windows (Full Development)
1. Install Visual Studio with required components (listed above)
2. Install CMake 3.25+
3. Enable Developer Mode
4. Clone repository
5. Run `cmake .` to generate solution

### Linux (Documentation/Validation Only)
1. Install Python 3.8+
2. Install docs tools: `pip install mkdocs-mermaid2-plugin mkdocs`
3. Clone repository
4. Run validation commands as needed

Remember: **This is a Windows-focused project**. While some tasks can be performed on Linux, full WSL development requires Windows with Visual Studio.