# WSLC aggregate virtiofs benchmark

This harness compares virtiofs request queue counts while multiple WSLC bind mounts use distinct children of one aggregate virtiofs device. It runs pinned, offline Git clone and source extraction workloads concurrently across 1, 2, 4, 8, or 16 host shares.

## Prerequisites

- Build and deploy the WSL tree containing the configurable `experimental.virtioFsQueueCount` setting.
- Ensure `wslc.exe` can create containers and reach GitHub while building the fixture image.
- Run PowerShell from an account that can use WSLC.

The image build fetches TypeScript `v5.8.3` once and selects its `src` tree plus real project test cases, about 1,500 entries. It stores that subset as both a bare repository and an uncompressed source archive. The Git workload checks out a clone, while the source extraction workload creates the same tree without Git processing. Timed runs use only data stored in the image, so network latency is excluded and no npm registry access is required.

## Run

From the repository root:

```powershell
.\tools\benchmarks\virtiofs\Run-WslcVirtioFsBenchmark.ps1
```

For a short smoke run:

```powershell
.\tools\benchmarks\virtiofs\Run-WslcVirtioFsBenchmark.ps1 `
    -QueueCounts 1,4 `
    -ShareCounts 1,4 `
    -Workloads git-clone `
    -WarmupRuns 0 `
    -Repetitions 1
```

Use `-SkipImageBuild` after the `wslc-virtiofs-benchmark:local` image has been built. Use `-WslcPath` to select a deployed or non-Debug `wslc.exe`.

The script temporarily replaces `%LOCALAPPDATA%\wslc\settings.yaml`, restores its original bytes in a `finally` block, and terminates the default WSLC session between queue counts. Do not edit the settings file while the benchmark is running.

## Results

`raw.csv` contains one row per share, including its duration, aggregate device ID, and filesystem root. `summary.csv` contains mean, median, and p95 batch duration plus aggregate shares per second for each queue count, share count, and workload.

The container rejects a run unless all shares report the same `MAJ:MIN` and distinct `FSROOT` values. This verifies that the workload uses separate children on one aggregate virtiofs device.