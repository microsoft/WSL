# WER proxy reporting experiment

Answers two questions about reporting a crash on behalf of another process, which is what WSL needs
if `wslservice` is going to submit reports for Linux processes:

1. Can one process submit a `WSLAppCrash` report that describes a different process, and does that
   report reach Watson under the target's identity?
2. Does WER suspend the target while it does so?

Two binaries:

- **`spintarget.cpp` → `winget.exe` (Process A)** — the process being reported against. It spins on
  `QueryPerformanceCounter` across several threads and records every gap in its own execution. It is
  named `winget.exe` and carries a matching version resource so the resulting bucket lands with a
  team that owns that name, which also exercises CompCentral routing. It is unsigned, which may
  affect that routing.
- **`proxyreport.cpp` → `proxyreport.exe` (Process B)** — builds and submits the report. The
  report's `WER_REPORT_INFORMATION.hProcess` and the collected dump both refer to Process A.

Both print raw QPC values. QPC is a system wide timebase, so a gap reported by A can be lined up
with the exact WER call in B that caused it.

Gaps are tracked per thread. One thread stalling is ordinary scheduling; the same interval appearing
on *every* thread means the process itself was stopped, and that is what the summary flags.

## Building

Standalone — it does not depend on anything else in this repository and does not participate in the
WSL build.

```powershell
cmake -S tools\werproxyexperiment -B tools\werproxyexperiment\build
cmake --build tools\werproxyexperiment\build --config Debug
```

To report against a different name, reconfigure with `-DSPINTARGET_NAME=<name>`. Do not put the
output directory on `PATH`.

## Running

```powershell
cd tools\werproxyexperiment\build\Debug

# Process A. It prints its pid and then a running worst-gap line every second.
.\winget.exe --duration 45 --threads 3 --threshold 2

# Process B, from a second window, against that pid.
.\proxyreport.exe --pid <pid> --dump mini
```

Then let A run to completion and read its summary.

To separate dump collection from the rest of the report, run the same thing with `--dump none`. To
attach a payload without reading anything out of the target, add `--add-file <path>`.

### Parameters

`--param <n> <name> <value>` overrides any of P0-P9, and `--no-params` sends none.

The defaults are the schema registered for `WSLAppCrash`. Watson numbers these P1-P7;
`WerReportSetParameter` indexes the same slots from zero:

| Watson | Index | Name |
| --- | --- | --- |
| P1 | 0 | `AppName` |
| P2 | 1 | `AppVersion` |
| P3 | 2 | `ModuleName` |
| P4 | 3 | `ModuleVersion` |
| P5 | 4 | `ModuleOffset` |
| P6 | 5 | `ExceptionCode` |
| P7 | 6 | `Flags` |

The set has to match exactly. Submitting a different number of parameters gets the report rejected;
see the results below.

### Finding the report

`WerReportSubmit` returns `WerReportQueued`, so the report is written to disk first. It lands under
`%ProgramData%\Microsoft\Windows\WER` (machine wide) or `%LOCALAPPDATA%\Microsoft\Windows\WER`
depending on the submitting account, in a directory named after the target, for example
`ReportArchive\AppCrash_winget.exe_<hash>_...`.

```powershell
Get-ChildItem "$env:ProgramData\Microsoft\Windows\WER","$env:LOCALAPPDATA\Microsoft\Windows\WER" `
    -Recurse -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 5 FullName
```

Reading `Report.wer` under `%ProgramData%` requires an elevated shell. The Application event log
carries the same verdict and needs no elevation, which is the easier way to check the outcome:

```powershell
Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='Windows Error Reporting';
    StartTime=(Get-Date).AddMinutes(-10)} |
  ForEach-Object { ($_.Message -split "`n" |
    Select-String '^Event Name:|^Fault bucket|^Cab Id') -join ' | ' }
```

To push the queue out rather than waiting for the scheduler:

```powershell
Start-ScheduledTask -TaskPath '\Microsoft\Windows\Windows Error Reporting\' -TaskName QueueReporting
```

## Results

Measured on an x64 dev machine, Process A spinning on three threads with a 2 ms gap threshold.

### Reaching Watson

Reports reach the service within a minute or so of submission even though `WerReportSubmit` returns
`WerReportQueued`, and the Application log records the verdict.

| Event name | Parameters | Verdict |
| --- | --- | --- |
| `WSLAppCrash` | 8, including `AppTimeStamp` and `ModuleTimeStamp` | `INVALID_REQUEST`, type 0 |
| `WSLAppCrash` | none | `INVALID_REQUEST`, type 0 |
| `WslProxyTest-<timestamp>`, unregistered | 8, as above | `INVALID_REQUEST`, type 0 |
| `APPCRASH` | 8, as above | bucket `2182272543438899849`, type 4 |
| **`WSLAppCrash`** | **the registered 7** | **bucket `1425583437259676584`, type 5** |

`INVALID_REQUEST` is the service rejecting the *event*, not the parameter values: sending no
parameters at all changed nothing, and the identical values bucketed fine under `APPCRASH`. What
fixed it was matching the registered signature exactly. The two timestamp parameters copied from the
standard Windows application crash schema are not part of `WSLAppCrash`, and their presence was
enough to get every report thrown away.

Both successful reports came back with a non-zero Cab Id, so the dump is uploaded and retrievable
rather than the event being recorded on its own.

Nothing else had to be true for this to work: the target was an unsigned dev build, the version and
timestamp values were fabricated, and the report described a process other than the submitter.

### Cost to the reported process

With `--dump mini`:

| Call | Duration |
| --- | --- |
| `WerReportCreate` | 77 ms |
| `WerReportSetParameter` | 0.2 ms |
| `WerReportAddDump` | 0.3 ms |
| `WerReportSubmit` | 2625 ms |

Process A was fully suspended twice, 33 ms and 46 ms, each seen on all three threads, and both
inside the `WerReportSubmit` window. A later run showed the same shape, two pauses of about 21 ms.

With `--dump none`, Process A recorded no interruption at all during the WER sequence.

With `--add-file` and `--dump none`, which is how an ELF core from inside the utility VM is
attached, Process A was likewise never suspended, even though `hProcess` still pointed at it:

| Call | Duration |
| --- | --- |
| `WerReportCreate` | 86 ms |
| `WerReportSetParameter` | 0.2 ms |
| `WerReportAddFile` | 0.3 ms |
| `WerReportSubmit` | 2316 ms |

Across 30 seconds on three threads A recorded three gaps, each on a single thread and none
overlapping the submit window; the largest was 7 ms of ordinary scheduling. Compare that with the
`--dump mini` run above, where the pauses appeared on all three threads at once.

So:

- **Proxy reporting works.** The report is filed under the target's name and image path, and the
  faulting address resolves inside the target's own module.
- **Only `WerReportAddDump` suspends the target**, for tens of milliseconds. The call itself merely
  registers the request; the suspension happens later, during `WerReportSubmit`. Attaching an
  existing file with `WerReportAddFile` costs the target nothing, so a report can carry a dump
  without ever stopping the process it names.
- `WerReportCreate` costs ~80 ms and `WerReportSubmit` a couple of seconds in the *reporting*
  process, neither of which touches the target. Note that `WerReportSubmit` takes that long even
  when it only queues the report locally, so callers that submit on a thread which has other work
  to do should expect to lose a couple of seconds to it.
