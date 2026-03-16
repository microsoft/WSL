---
description: 'Guidelines for the WSL Linux unit tests Makefile'
applyTo: 'test/linux/unit_tests/Makefile'
---

# Makefile — WSL Linux Unit Tests

WSL has a single Makefile at `test/linux/unit_tests/Makefile` that builds the Linux-side unit test binary (`wsl_unit_tests`). This is a simple GCC-based build for C test code.

## Current Structure

- Compiler: GCC with `-ggdb -Werror -Wno-format-truncation -Wno-format-overflow -D_GNU_SOURCE=1`
- Link flags: `-pthread -lutil -lmount`
- Architecture-aware: x86_64 includes `select.o`; aarch64 excludes it
- Targets: `all` (builds test binary), `clean` (removes objects and binary)

## Guidelines

- Maintain the existing flat structure — all `.o` files listed explicitly in `UNIT_TEST_OBJECTS`.
- When adding a new test file, add its `.o` to `UNIT_TEST_OBJECTS` (or architecture-specific lists if needed).
- Keep architecture conditionals for syscalls not available on all platforms (see `__NR_select` comment).
- Use `.PHONY` for `clean` and `all` (already declared).
- Use tab characters for recipe lines (not spaces).
- Keep `-Werror` to catch warnings as errors.
- Do not add complex build logic — the main WSL build system is CMake.
