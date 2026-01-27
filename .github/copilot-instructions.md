# Windows Subsystem for Linux (WSL)

**ALWAYS reference these instructions first and fallback to search or bash commands only when you encounter unexpected information that does not match the info here.**

WSL is the Windows Subsystem for Linux - a compatibility layer for running Linux binary executables natively on Windows. This repository contains the core Windows components that enable WSL functionality.

## Working Effectively

### Critical Platform Requirements
- **Full builds ONLY work on Windows** with Visual Studio and Windows SDK 26100
- **DO NOT attempt to build the main WSL components on Linux** - they require Windows-specific APIs, MSBuild, and Visual Studio toolchain
- Many validation and development tasks CAN be performed on Linux (documentation, formatting, Python validation scripts)

### Windows Build Requirements (Required for Full Development)
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
4. **NEVER CANCEL: Build takes 20-45 minutes on typical hardware. Set timeout to 60+ minutes.**

Build parameters:
- `cmake . -A arm64` - Build for ARM64
- `cmake . -DCMAKE_BUILD_TYPE=Release` - Release build  
- `cmake . -DBUILD_BUNDLE=TRUE` - Build bundle msix package (requires ARM64 built first)

### Deploying WSL (Windows Only)  
- Install MSI: `bin\<platform>\<target>\wsl.msi`
- OR use script: `powershell tools\deploy\deploy-to-host.ps1`
- For Hyper-V VM: `powershell tools\deploy\deploy-to-vm.ps1 -VmName <vm> -Username <user> -Password <pass>`

## Cross-Platform Development Tasks

### Documentation (Works on Linux/Windows)
- Install tools: `pip install mkdocs-mermaid2-plugin mkdocs --break-system-packages`
- Build docs: `mkdocs build -f doc/mkdocs.yml`
- **Build time: ~0.5 seconds. Set timeout to 5+ minutes for safety.**
- Output location: `doc/site/`
- **Note**: May show warnings about mermaid CDN access on restricted networks

### Code Formatting and Validation (Works on Linux/Windows)
- Format check: `clang-format --dry-run --style=file <files>`
- Apply formatting: `clang-format -i --style=file <files>`
- Format all source: `powershell formatsource.ps1` (available at repo root after running `cmake .`)
- Validate copyright headers: `python3 tools/devops/validate-copyright-headers.py`
  - **Note**: Will report missing headers in generated/dependency files (_deps/), which is expected
- Validate localization: `python3 tools/devops/validate-localization.py` 
  - **Note**: Only works after Windows build (requires localization/strings/en-us/Resources.resw)

### Distribution Validation (Limited on Linux)
- Validate distribution info: `python3 distributions/validate.py distributions/DistributionInfo.json`
- **Note**: May fail on Linux due to network restrictions accessing distribution URLs

## Testing

### Unit Tests (Windows Only - TAEF Framework)

**CRITICAL: ALWAYS build the ENTIRE project before running tests:**
```powershell
# Build everything first - this is required!
cmake --build . -- -m

# Then run tests
bin\<platform>\<target>\test.bat
```

**Why full build is required:**
- Tests depend on multiple components (libwsl.dll, wsltests.dll, wslservice.exe, etc.)
- Partial builds (e.g., only `configfile` or `wsltests`) will cause test failures
- Changed components must be built together to ensure compatibility
- **DO NOT skip the full build step even if only one file changed**

Test execution:
- Run all tests: `bin\<platform>\<target>\test.bat`
- **NEVER CANCEL: Full test suite takes 30-60 minutes. Set timeout to 90+ minutes.**
- Run subset: `bin\<platform>\<target>\test.bat /name:*UnitTest*`
- Run specific test: `bin\<platform>\<target>\test.bat /name:<class>::<test>`
- WSL1 tests: Add `-Version 1` flag
- Fast mode (after first run): Add `-f` flag (requires `wsl --set-default test_distro`)
- **Requires Administrator privileges** - test.bat will fail without admin rights

Test debugging:
- Wait for debugger: `/waitfordebugger`
- Break on failure: `/breakonfailure`
- Run in-process: `/inproc`

### Linux Unit Tests (Linux Only)
- Location: `test/linux/unit_tests/`
- Build script: `test/linux/unit_tests/build_tests.sh`
- **Note**: Requires specific Linux build environment setup not covered in main build process

## Validation Scenarios

### Always Test These After Changes:
1. **Documentation Build**: Run `mkdocs build -f doc/mkdocs.yml` and verify no errors
2. **Code Formatting**: Run `clang-format --dry-run --style=file` on changed files
3. **Windows Build** (if on Windows): Full cmake build cycle
4. **Distribution Validation**: Run Python validation scripts on any distribution changes

### Manual Validation Requirements
- **Windows builds**: Install MSI and test basic WSL functionality (`wsl --version`, `wsl -l`)
- **Documentation changes**: Review generated HTML in `doc/site/`
- **Distribution changes**: Test with actual WSL distribution installation

## Repository Navigation

### Key Directories
- `src/windows/` - Main Windows WSL service components
- `src/linux/` - Linux-side WSL components  
- `src/shared/` - Shared code between Windows and Linux
- `test/windows/` - Windows-based tests (TAEF framework)
- `test/linux/unit_tests/` - Linux unit test suite
- `doc/` - Documentation source (MkDocs)
- `tools/` - Build and deployment scripts
- `distributions/` - Distribution validation and metadata

### Key Files
- `CMakeLists.txt` - Main build configuration
- `doc/docs/dev-loop.md` - Developer build instructions
- `test/README.md` - Testing framework documentation
- `CONTRIBUTING.md` - Contribution guidelines
- `.clang-format` - Code formatting rules
- `UserConfig.cmake.sample` - Optional build customizations

### Frequently Used Commands (Platform-Specific)

#### Windows Development:
```bash
# Initial setup
cmake .
cmake --build . -- -m # 20-45 minutes, NEVER CANCEL

# Deploy and test  
powershell tools\deploy\deploy-to-host.ps1
wsl --version

# Run tests
bin\x64\debug\test.bat # 30-60 minutes, NEVER CANCEL
```

#### Cross-Platform Validation:
```bash  
# Documentation (0.5 seconds)
mkdocs build -f doc/mkdocs.yml

# Code formatting  
find src -name "*.cpp" -o -name "*.h" | xargs clang-format --dry-run --style=file

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

## Critical Timing and Timeout Guidelines

**NEVER CANCEL these operations - always wait for completion:**

- **Full Windows build**: 20-45 minutes (set timeout: 60+ minutes)
- **Full test suite**: 30-60 minutes (set timeout: 90+ minutes)
- **Unit test subset**: 5-15 minutes (set timeout: 30+ minutes)
- **Documentation build**: ~0.5 seconds (set timeout: 5+ minutes)
- **Distribution validation**: 2-5 minutes (set timeout: 15+ minutes)

## CI/CD Integration

### GitHub Actions
- **distributions.yml**: Validates distribution metadata (Linux)
- **documentation.yml**: Builds and deploys docs (Linux) 
- **modern-distributions.yml**: Tests modern distribution support

### Pre-commit Validation
Always run before committing:
1. `clang-format --dry-run --style=file` on changed C++ files
2. `python3 tools/devops/validate-copyright-headers.py` (ignore _deps/ warnings)
3. `mkdocs build -f doc/mkdocs.yml` if documentation changed
4. Full Windows build if core components changed

**Note**: The `.gitignore` file properly excludes build artifacts (*.sln, *.dll, *.pdb, obj/, bin/, etc.) - do not commit these files.

## Development Environment Setup

### Windows (Full Development)
1. Install Visual Studio with required components (listed above)
2. Install CMake 3.25+
3. Enable Developer Mode
4. Clone repository
5. Run `cmake .` to generate solution

### Linux (Documentation/Validation Only)  
1. Install Python 3.8+
2. Install clang-format
3. Install docs tools: `pip install mkdocs-mermaid2-plugin mkdocs`
4. Clone repository
5. Run validation commands as needed

Remember: **This is a Windows-focused project**. While some tasks can be performed on Linux, full WSL development requires Windows with Visual Studio.