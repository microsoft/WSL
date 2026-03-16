# Windows Subsystem for Linux (WSL)

**ALWAYS reference these instructions first and fallback to search or bash commands only when you encounter unexpected information that does not match the info here.**

WSL is the Windows Subsystem for Linux — a compatibility layer for running Linux binary executables natively on Windows. This repository contains the core Windows and Linux components that enable WSL functionality.

## Documentation Links

| Resource | URL |
|----------|-----|
| User documentation | https://learn.microsoft.com/windows/wsl/ |
| Developer documentation | https://wsl.dev/ |
| Build, test, deploy guide | https://wsl.dev/dev-loop/ |
| Architecture overview | https://wsl.dev/technical-documentation/ |
| Boot process deep-dive | https://wsl.dev/technical-documentation/boot-process/ |
| Debugging guide | https://wsl.dev/debugging/ |
| Interop (Windows ↔ Linux) | https://wsl.dev/technical-documentation/interop/ |
| Drvfs & Plan9 | https://wsl.dev/technical-documentation/drvfs/ |
| Systemd integration | https://wsl.dev/technical-documentation/systemd/ |

→ For full architecture details (component diagram, boot sequence, source file map), see `.github/instructions/architecture.instructions.md` (auto-applied when editing `src/**`).

## How to Build (Windows Only)

**Critical**: Full builds ONLY work on Windows with Visual Studio and Windows SDK 26100. DO NOT attempt to build main components on Linux.

```powershell
# 1. Generate Visual Studio solution
cmake .

# 2. Build (20–45 min — NEVER CANCEL)
cmake --build . -- -m
```

| Parameter | Purpose |
|-----------|---------|
| `cmake . -A arm64` | ARM64 build |
| `cmake . -DCMAKE_BUILD_TYPE=Release` | Release build |
| `cmake . -DBUILD_BUNDLE=TRUE` | Bundle MSIX (ARM64 must be built first) |

**Faster iteration**: Copy `UserConfig.cmake.sample` → `UserConfig.cmake`, uncomment `WSL_DEV_BINARY_PATH`. Also see `WSL_BUILD_THIN_PACKAGE` and `WSL_POST_BUILD_COMMAND`.

**Deploy**: `bin\<platform>\<target>\wsl.msi` or `powershell tools\deploy\deploy-to-host.ps1`

→ See `.github/instructions/build-deploy.instructions.md` for full details (ARM64 setup, Hyper-V deployment).

## How to Test

**CRITICAL: Always do a full build before running tests.**

```powershell
cmake --build . -- -m                                  # Full build first!
bin\x64\debug\test.bat                                 # All tests (30–60 min)
bin\x64\debug\test.bat /name:*UnitTest*                # Subset
bin\x64\debug\test.bat /name:UnitTests::UnitTests::ModernInstall  # Specific test
```

- Requires **Administrator privileges**
- Fast mode (skip install): `wsl --set-default test_distro` then add `-f` flag
- WSL1 tests: add `-Version 1`
- Debug: `/waitfordebugger`, `/breakonfailure`, `/inproc`

**Linux unit tests**: `make` in `test/linux/unit_tests/` (GCC, produces `wsl_unit_tests`)

→ See `.github/instructions/testing.instructions.md` for full details.

## How to Debug

```powershell
# ETL tracing
wpr -start diagnostics\wsl.wprp -filemode
# [reproduce issue]
wpr -stop logs.ETL
# Profiles: WSL (default), WSL-Storage, WSL-Networking, WSL-HvSocket
# Example: wpr -start diagnostics\wsl.wprp!WSL-Networking -filemode

# Collect logs
powershell diagnostics\collect-wsl-logs.ps1
powershell diagnostics\collect-wsl-logs.ps1 -LogProfile networking

# Debug console (add to %USERPROFILE%\.wslconfig)
# [wsl2]
# debugConsole=true

# Debug shell (for gns, mini_init — root namespace)
wsl --debug-shell

# Windows debugger symbols
# Available under bin/<platform>/<target>/
```

**Key ETL providers**: `Microsoft.Windows.Lxss.Manager` (wslservice), `Microsoft.Windows.Subsystem.Lxss` (wsl.exe et al.), `Microsoft.Windows.Plan9.Server`

→ See `.github/instructions/debugging.instructions.md` for full provider event details and debugger attachment.

## Best Practices & Guidelines

### Code Formatting (Pre-Commit)

1. `clang-format --dry-run --style=file` on changed C/C++ files
2. `python3 tools/devops/validate-copyright-headers.py` (ignore `_deps/` warnings)
3. `mkdocs build -f doc/mkdocs.yml` if documentation changed
4. Full Windows build if core components changed

- Format all: `powershell .\FormatSource.ps1 -ModifiedOnly $false` (requires `cmake .` first — script is generated from `tools/FormatSource.ps1.in`)
- Auto-check on commit: `tools\SetupClangFormat.bat`

### Validation After Changes

| What changed | Validation |
|--------------|------------|
| C/C++ source | clang-format + copyright headers + full build + tests |
| C# (wslsettings) | Full build (includes wslsettings) |
| Documentation | `mkdocs build -f doc/mkdocs.yml`, review `doc/site/` |
| Distributions | `python3 distributions/validate.py distributions/DistributionInfo.json` |
| Localization | `python3 tools/devops/validate-localization.py` (after build) |

### Build Artifacts

The `.gitignore` excludes build artifacts (`*.sln`, `*.dll`, `*.pdb`, `obj/`, `bin/`, etc.) — do not commit these.

### CI/CD

| Pipeline | Type | Purpose |
|----------|------|---------|
| `distributions.yml` | GitHub Actions | Validate distribution metadata (Linux) |
| `documentation.yml` | GitHub Actions | Build and deploy docs (Linux) |
| `modern-distributions.yml` | GitHub Actions | Test modern distribution support |
| `.pipelines/wsl-build-pr.yml` | Azure DevOps | PR validation builds |
| `.pipelines/wsl-build-nightly-onebranch.yml` | Azure DevOps | Nightly builds |
| `.pipelines/wsl-build-release-onebranch.yml` | Azure DevOps | Release builds |

### Timing — NEVER Cancel These

| Operation | Duration | Set Timeout |
|-----------|----------|-------------|
| Full Windows build | 20–45 min | 60+ min |
| Full test suite | 30–60 min | 90+ min |
| Unit test subset | 5–15 min | 30+ min |
| Documentation build | ~0.5 sec | 5+ min |
| Distribution validation | 2–5 min | 15+ min |

## Repository Navigation

### Key Directories

| Directory | Content |
|-----------|---------|
| `src/windows/service/` | wslservice.exe — core WSL service |
| `src/windows/wsl/` | wsl.exe — CLI entrypoint |
| `src/windows/common/` | Shared Windows utilities (relay, COM, HCS schema) |
| `src/windows/libwsl/` | wslapi.dll — public API |
| `src/windows/wslsettings/` | WinUI 3 settings app (C#) |
| `src/windows/wslhost/` | Background process management |
| `src/windows/wslrelay/` | Network/debug console relay |
| `src/linux/init/` | mini_init, init, gns, plan9, session leader, relay, localhost |
| `src/linux/` | Linux sub-components (inc, mountutil, netlinkutil, plan9) |
| `src/shared/` | Cross-platform code (config, message definitions) |
| `test/windows/` | Windows tests (TAEF framework) |
| `test/linux/unit_tests/` | Linux unit tests (GCC/Makefile) |
| `doc/` | MkDocs documentation source |
| `tools/` | Build, deploy, and DevOps scripts |
| `distributions/` | Distribution metadata and validation |
| `diagnostics/` | ETL trace profiles and log collection scripts |
| `.pipelines/` | Azure DevOps pipeline definitions |
| `.github/workflows/` | GitHub Actions workflows |

### Key Files

| File | Purpose |
|------|---------|
| `CMakeLists.txt` | Root build configuration |
| `UserConfig.cmake.sample` | Optional build customization template |
| `.clang-format` | Code formatting rules |
| `CONTRIBUTING.md` | Contribution guidelines |
| `src/windows/service/inc/wslservice.idl` | COM interface definition |
| `src/shared/inc/lxinitshared.h` | Cross-platform message definitions |
| `src/linux/init/main.cpp` | Linux main() entrypoint |
| `src/linux/init/init.cpp` | WslEntryPoint() — argv[0] dispatcher |

## Scoped Instruction Files

Detailed guidelines auto-apply based on the files you're editing:

| File | Scope | Content |
|------|-------|---------|
| `architecture.instructions.md` | `src/**` | Component deep-dives, source file map, boot sequence |
| `cpp.instructions.md` | `**/*.cpp, **/*.h, **/*.c` | C/C++ style, clang-format, copyright, WIL/GSL |
| `csharp.instructions.md` | `src/windows/wslsettings/**/*.cs` | WinUI 3, MVVM, .NET 8.0 conventions |
| `build-deploy.instructions.md` | `**/CMakeLists.txt, **/*.cmake, tools/deploy/**` | Build prereqs, ARM64, UserConfig, deployment |
| `testing.instructions.md` | `test/**` | TAEF framework, test commands, Linux unit tests |
| `debugging.instructions.md` | `diagnostics/**` | ETL tracing, providers, debugger attachment |
| `documentation.instructions.md` | `doc/**` | MkDocs build and validation |
| `distributions.instructions.md` | `distributions/**` | Distribution metadata validation |
| `python.instructions.md` | `**/*.py` | Python validation/DevOps script conventions |
| `azure-devops-pipelines.instructions.md` | `.pipelines/**/*.yml` | ADO pipeline guidelines |
| `makefile.instructions.md` | `test/linux/unit_tests/Makefile` | Linux unit test Makefile |
| `agents.instructions.md` | `**/*.agent.md` | Custom agent file guidelines |
| `agent-skills.instructions.md` | `.github/skills/**/SKILL.md` | Agent Skills guidelines |
| `prompt.instructions.md` | `**/*.prompt.md` | Copilot prompt file guidelines |
| `instructions.instructions.md` | `**/*.instructions.md` | Meta: writing instruction files |

## Development Environment Setup

### Windows (Full Development)
1. Install Visual Studio with required components (see Build section)
2. Install CMake 3.25+ (`winget install Kitware.CMake`)
3. Enable Developer Mode
4. Clone repository
5. `cmake .` to generate solution

### Linux (Documentation/Validation Only)
1. Install Python 3.8+, clang-format
2. `pip install mkdocs-mermaid2-plugin mkdocs`
3. Clone repository
4. Run validation commands as needed

**This is a Windows-focused project.** While some tasks work on Linux, full WSL development requires Windows with Visual Studio.