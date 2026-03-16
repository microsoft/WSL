---
description: 'C/C++ coding standards, formatting, and copyright headers for WSL source code'
applyTo: '**/*.cpp, **/*.h, **/*.c'
---

# C/C++ Development — WSL

## Code Formatting

- Every pull request must be clang-formatted before merge.
- Style defined in `.clang-format` at repo root (LLVM-based).
- Key settings: `ColumnLimit: 130`, `IndentWidth: 4`, `PointerAlignment: Left`, Allman-style braces for classes/functions/control flow.
- Format all source: `powershell .\FormatSource.ps1 -ModifiedOnly $false` (requires `cmake .` first — script is generated from `tools/FormatSource.ps1.in`)
- Format specific files: `clang-format -i --style=file <files>`
- Check without modifying: `clang-format --dry-run --style=file <files>`
- Auto-check on commit: run `tools\SetupClangFormat.bat` once to install the git hook.

## Copyright Headers

All source files require a copyright header. Two styles are used:

**Modern (single-line):**
```cpp
// Copyright (C) Microsoft Corporation. All rights reserved.
```

**Traditional Windows (multi-line):**
```cpp
/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    <FileName>.cpp

Abstract:

    <Description of the file's purpose.>

--*/
```

Match the style used in surrounding files. Validate: `python3 tools/devops/validate-copyright-headers.py` (reports `_deps/` issues — expected, ignore).

## Pre-Commit Checklist

1. `clang-format --dry-run --style=file` on changed C/C++ files
2. `python3 tools/devops/validate-copyright-headers.py` (ignore `_deps/` warnings)
3. Full Windows build if core components changed

## Key Libraries

| Library | Version | Usage |
|---------|---------|-------|
| **WIL** (Windows Implementation Library) | 1.0.251108.1 | RAII wrappers (`wil::unique_hkey`, `wil::unique_handle`, `wil::unique_cotaskmem_string`), COM helpers, error handling |
| **GSL** (Guidelines Support Library) | 4.0.0 | Buffer safety (`gsl::span`, `gsl::make_span`), contracts |
| **nlohmann/json** | 3.12.0 | JSON serialization for HCS VM config, networking requests |

## Conventions

- Windows code uses MSVC and C++ ATL; Linux code uses GCC.
- Shared code (`src/shared/`) must compile on both platforms.
- Header files (`.h`) are used for both C and C++; match the style of surrounding code.
- Use WIL RAII types instead of raw handles on Windows side (e.g., `wil::unique_hkey`, `wil::unique_fd` on Linux via `src/linux/inc/lxwil.h`).
- No `.editorconfig` in this repo — rely on `.clang-format` and git hooks.
