---
description: 'WSL test execution, TAEF framework, test debugging, and Linux unit tests'
applyTo: 'test/**'
---

# Testing — WSL

Reference: https://wsl.dev/dev-loop/

## TAEF Unit Tests (Windows Only)

**CRITICAL: ALWAYS build the ENTIRE project before running tests:**
```powershell
cmake --build . -- -m    # Full build required first!
bin\<platform>\<target>\test.bat
```

**Why full build is required:**
- Tests depend on multiple components (libwsl.dll, wsltests.dll, wslservice.exe, etc.)
- Partial builds will cause test failures
- **DO NOT skip the full build step even if only one file changed**

### Running Tests

| Command | Purpose |
|---------|---------|
| `bin\x64\debug\test.bat` | Run all tests (30–60 min, NEVER CANCEL) |
| `bin\x64\debug\test.bat /name:*UnitTest*` | Run unit test subset |
| `bin\x64\debug\test.bat /name:<class>::<test>` | Run specific test |
| `bin\x64\debug\test.bat -Version 1` | Run WSL1 tests |
| `bin\x64\debug\test.bat /name:*UnitTest* -f` | Fast mode (skip install, requires `wsl --set-default test_distro`) |

- Example: `bin\x64\debug\test.bat /name:UnitTests::UnitTests::ModernInstall`
- **Requires Administrator privileges** — test.bat will fail without admin rights.

### Fast Mode Setup
```powershell
wsl --set-default test_distro
bin\x64\debug\test.bat /name:*UnitTest* -f
```

### Test Debugging

| Flag | Purpose |
|------|---------|
| `/waitfordebugger` | Pause until a debugger is attached |
| `/breakonfailure` | Break on first test failure |
| `/inproc` | Run tests in-process |

See also: https://wsl.dev/debugging/

## Linux Unit Tests

- Location: `test/linux/unit_tests/`
- Build: `make` in `test/linux/unit_tests/` (GCC, produces `wsl_unit_tests` binary)
- Architecture-aware: x86_64 includes `select.o`; aarch64 excludes it
- Requires a Linux build environment

## Timeouts

| Operation | Duration | Set Timeout |
|-----------|----------|-------------|
| Full test suite | 30–60 min | 90+ min |
| Unit test subset | 5–15 min | 30+ min |
