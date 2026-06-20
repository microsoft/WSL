# WSLC Fuzzing

Fuzzing harnesses for WSLC, built with AddressSanitizer (ASAN) and libFuzzer for integration with OneFuzz.

## How It Works

When `WSL_BUILD_FUZZING=true`:
- The entire build is compiled with `/fsanitize=address` — ASAN detects memory errors at runtime.
- Fuzzing harness executables get `/fsanitize=fuzzer` — this links the libFuzzer runtime and enables SanitizerCoverage for coverage-guided mutation.

Reference: https://learn.microsoft.com/cpp/sanitizers/asan-building

## Adding a New Harness

1. Create `TargetName.cpp` implementing `LLVMFuzzerTestOneInput` and including `FuzzingHarness.h`
2. Register it in `CMakeLists.txt`:

```cmake
add_fuzzing_harness(MyNewFuzzing
    TARGET_NAME myTarget
    LINK_LIBRARIES somelib
    EXTRA_DEPENDENCIES "extra.dll" "extra.pdb")
```

The function handles compiler flags, PCH, and contributes one entry to `OneFuzzConfig.json`.
The entry is rendered from `OneFuzzConfigEntry.json.in`; the surrounding file comes from `OneFuzzConfig.json.in`.

## Building

Enable fuzzing in `UserConfig.cmake`:

```cmake
set(WSL_BUILD_FUZZING true)
```

Then:

```powershell
cmake . --fresh
cmake --build . --config Release --target WslcCliArgumentFuzzing -- -m
```

The fuzzing build cannot be mixed with a non-ASAN build in the same tree. Use `--fresh` when toggling `WSL_BUILD_FUZZING`.

## Harnesses

| Harness | Target | Description |
|---------|--------|-------------|
| `WslcCliArgumentFuzzing` | `ParseArgumentsStateMachine` | CLI argument parser — standalone, no service needed |
| `WslcSdkFuzzing` | WSLC SDK C API | Session + container creation flow via C functions |
| `WslcWinRtFuzzing` | WSLC SDK WinRT API | Same flow via C++/WinRT projection |

## OneFuzz Integration

The nightly pipeline (`.pipelines/fuzzing-stage.yml`) builds the harnesses + MSI, stages them into a flat drop, and submits them with `onefuzz-task@0`.
`OneFuzzConfig.json` holds one entry per harness; shared dependencies (`setup.ps1`, `wsl.msi`, the ASAN runtime) sit alongside the binaries in the drop.

### VM Setup

`setup.ps1` runs on the OneFuzz VM before fuzzing. It enables Hyper-V, enables WSL, and installs `wsl.msi` from the build drop. `RebootAfterSetup` is enabled so features activate before the harness runs.

### Seed Corpus (optional, not yet wired up)

The harnesses run without a seed corpus — libFuzzer starts from an empty corpus and generates its own inputs.
Seeds only speed up the initial coverage ramp (most useful for the structured SDK/WinRT
inputs).

To add seeding later, give each entry a `SeedCorpusContainer` in `OneFuzzConfigEntry.json.in` and upload the generated seeds to that container using the OneFuzz CLI.
`generate-seeds.ps1` produces a corpus per target; the same directories are used for local replay.

### Drop Layout

```
fuzzing/
├── OneFuzzConfig.json
├── clang_rt.asan_dynamic-x86_64.dll
├── setup.ps1
├── wsl.msi
├── <harness1>.exe
├── <harness1>.pdb
├── <harness1 dependencies>
├── ...
```

## Local Testing

To build a standalone replay binary (no libFuzzer runtime), add to your `UserConfig.cmake`:

```cmake
set(WSL_ENABLE_FUZZING_REPLAY true)
```

Then rebuild. The harness will include a `main()` that replays corpus files:

```powershell
WslcCliArgumentFuzzing.exe path\to\crash-input
```
