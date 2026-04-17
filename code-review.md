# WSL Codebase Deep Review — April 2026

## Executive Summary

A comprehensive deep-dive review of the entire WSL codebase identified **44 issues** across
7 major subsystems. **13 confirmed bugs** have been fixed on the `copilot/code-review-fixes`
branch (plus 8 more are already addressed by open PR #40197). The remaining findings are
documented below for future triage.

## Overlap with PR #40197 ("Fix batch of minor bugs")

PR #40197 already fixes the following issues that were independently identified by this review:
- `mountutil.c` — Missing `break` in `MountFieldDevice` switch case
- `util.cpp` — Dead code path: `Result=-1` always jumps to `ErrorExit`
- `DistributionRegistration.cpp` — Duplicate `Property::Flags` write
- `prettyprintshared.h` — Off-by-one comma logic
- `message.h` — `assert()` instead of runtime check + unused `m_offset` member
- `configfile.cpp` — `||` should be `&&` in malformed value check + wrong quote char
- `relay.cpp` — Off-by-one in `WaitForMultipleObjects` result check
- `filesystem.cpp` — Missing `hostname.resize()` before `CleanHostname`
- `lxwil.h` — `THROW_UNEXCEPTED` typo + `pipe2()` error check `< -1` → `< 0`
- `p9util.cpp` — `getpwuid_r`/`getgrnam_r` error check `< 0` → `!= 0`

**These are NOT duplicated in this branch.**

---

## Bugs Fixed on This Branch

### 1. CRITICAL — Wrong test handler for `get_addr_info`
- **File**: `test/linux/unit_tests/unittests.c:36`
- **Fix**: `GetSetIdTestEntry` → `GetAddrInfoTestEntry`

### 2. MEDIUM — `%s` format specifier in fmt-style `LOG_ERROR`
- **File**: `src/shared/inc/SocketChannel.h:345`
- **Fix**: `%s` → `{}`

### 3. MEDIUM — `readlinkat` return stored as `int` instead of `ssize_t`
- **File**: `src/linux/plan9/p9file.cpp:853`
- **Fix**: `int result` → `ssize_t result`

### 4. MEDIUM — `ungetwc(WEOF)` is undefined on some implementations
- **File**: `src/shared/configfile/configfile.cpp:376`
- **Fix**: Guard with `if (nextc != WEOF)` before `ungetwc()`

### 5. MEDIUM — SIGCHLD race window in init
- **File**: `src/linux/init/init.cpp:2390-2401`
- **Fix**: Block SIGCHLD via `sigprocmask` before calling `signal(SIGCHLD, SIG_DFL)`

### 6. LOW — Duplicated signal skip list
- **File**: `src/linux/init/util.cpp:2593-2604,2640-2651`
- **Fix**: Extracted `SkipSignal()` helper used by both `UtilSaveSignalHandlers` and
  `UtilSetSignalHandlers`

### 7. HIGH (Test) — `accept()` blocks indefinitely
- **File**: `test/windows/NetworkTests.cpp:1992`
- **Fix**: Set `SO_RCVTIMEO` timeout before `accept()` (resolves TODO)

### 8. MEDIUM (Test) — Silent cleanup failures in DrvFsTests
- **File**: `test/windows/DrvFsTests.cpp:66-121`
- **Fix**: Wrapped `DeleteFileW`/`RemoveDirectory` calls with `LOG_IF_WIN32_BOOL_FALSE`

### 9. HIGH (Security) — Command injection via `Invoke-Expression`
- **File**: `tools/test/copy_and_build_tests.ps1:55`
- **Fix**: Replaced with call operator `&`

### 10. MEDIUM (Security) — Unquoted command passed to `wsl.exe`
- **File**: `tools/test/test-setup.ps1:125-127`
- **Fix**: Route through `bash -c` for proper argument handling

### 11. MEDIUM — Empty `SecureString` when password not provided
- **File**: `tools/deploy/deploy-to-vm.ps1:16-18`
- **Fix**: Prompt via `Read-Host -AsSecureString` instead of creating empty SecureString

### 12. MEDIUM — `FileSystemWatcher` resource leak
- **File**: `src/windows/wslsettings/Services/WslConfigService.cs:32-36`
- **Fix**: Added `_wslConfigFileSystemWatcher?.Dispose()` in destructor

---

## Findings Deferred (Require Architectural Changes)

### Lock Ordering Risk — `LxssUserSessionFactory.cpp`
- **Severity**: High
- **Issue**: Comment states `g_sessionTerminationLock` must precede `g_sessionLock`, but
  `ClearSessionsAndBlockNewInstances()` may violate this in some code paths
- **Why deferred**: Requires thorough deadlock analysis across all callers

### Use-After-Free Risk — `LxssHttpProxy.cpp:143-212`
- **Severity**: High
- **Issue**: `s_GetProxySettingsExCallback` captures `HttpProxyStateTracker*` as raw pointer;
  if tracker is destroyed while WinHttp callback is pending, use-after-free occurs
- **Why deferred**: Requires `shared_ptr` refactor of callback context lifetime

### TOCTOU Symlink Race — `plan9/p9file.cpp:197-240`
- **Severity**: High
- **Issue**: `File::Walk` validates path then operates on it; symlink replacement possible
  between validation and use. Acknowledged by TODO comments in source.
- **Why deferred**: Requires FD-based walk operations with chroot

### Test Order Dependency — `UnitTests.cpp:102-103`
- **Severity**: Medium
- **Issue**: `ExportDistro` must run first; TAEF doesn't guarantee method ordering
- **Why deferred**: Needs test to be self-contained with pre-cleanup step

---

## Methodology

- 7 parallel review agents scanned: `src/windows/service/`, `src/windows/common/`,
  `src/shared/`, `src/linux/`, `src/windows/wslc*`, config/installer/tools, and `test/`
- Open PRs checked for overlap (PR #40197 covers 10+ findings)
- All findings verified against actual source before inclusion
- Fixes limited to confirmed, safe-to-change bugs with clear intent
- `FormatSource.ps1` passed on all changes
- Full Windows build verified (pre-existing proxystub/msix errors only)
