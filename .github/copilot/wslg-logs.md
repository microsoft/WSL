# Analyzing WSLg logs

WSLg is the component that runs graphical (GUI) and audio Linux applications on WSL. Its
source lives in a separate repository: https://github.com/microsoft/wslg. The canonical list
of diagnostics to request for a WSLg bug is the WSLg repo bug report template
(`.github/ISSUE_TEMPLATE/bug_report.yml` in microsoft/wslg).

`diagnostics/collect-wsl-logs.ps1` collects the logs into a `wslg/` subfolder of the log archive
(via `wsl.exe --system --user root`, with a timeout so a wedged service can't hang collection), so a
standard WSL log collection already contains the log files below. Crash dumps (`dumps/`,
`wsl-crashes/`) are only collected when the script is run with `-Dump`.

## Architecture (just enough to read the logs)

WSLg runs a **system distro** (a small Azure Linux VM, separate from the user distro) that hosts:
- **weston** (Wayland compositor) with the `rdp-backend.so` / `rdprail-shell.so` modules
- **Xwayland** for X11 apps
- **pulseaudio** with an RDP sink/source for audio
- **FreeRDP** to stream the surfaces over an hvsocket to the Windows RDP client (`msrdc.exe`)

`weston.log` and `wlog.log` are written by weston/FreeRDP, `pulseaudio.log` by pulseaudio, and
`stderr.log` is the combined stderr of `WSLGd` and the processes it launches.

## Files (in the `wslg/` folder, sourced from `/mnt/wslg`)

| File | Contents |
|---|---|
| `versions.txt` | WSLg version, architecture, build date, component git hashes (weston, FreeRDP, mesa, pulseaudio) |
| `weston.log` | Weston compositor + RDP backend log |
| `wlog.log` | FreeRDP (WLog) log |
| `pulseaudio.log` | PulseAudio log |
| `stderr.log` | `WSLGd` and child-process stderr |
| `dumps/` | Legacy WSLg crash dumps (older builds only; collected with `-Dump`) |
| `wsl-crashes/` | Host-side WSLg crash dumps copied from `%TEMP%\wsl-crashes` (newer builds, e.g. `core.weston`; collected with `-Dump`) |

## Reading tips and common signatures

- **`weston.log` is truncated on every boot.** Weston is started with `--log=/mnt/wslg/weston.log`
  and opens it truncating, so the file only ever contains the **latest** system-distro boot. To
  tell whether weston restarted, compare the boot timestamp on the first line (`weston 9.0.0 ...`)
  across snapshots, or correlate with `dmesg`.

- **System distro cycling / teardown.** In `dmesg` (collected via the ETL `GuestLog` events or
  the debug console), this pair means the WSLg system distro's `init` exited and the distro was
  torn down (idle timeout or shutdown), which also restarts weston:
  ```
  Exception: Operation canceled @p9io.cpp:258 (AcceptAsync)
  WSL (1 - init()) ERROR: InitEntryUtilityVm:2551: Init has exited. Terminating distribution
  ```
  Repeating every ~25-60s with no GUI client attached is usually normal idle teardown, not a crash.
  A genuine weston crash instead leaves a core dump in `wsl-crashes/` (or `dumps/`).

- **No GPU acceleration (software rendering).** This means the virtual GPU (`/dev/dxg`, d3d12 mesa)
  is not usable, so rendering falls back to CPU:
  ```
  Xwayland glamor: GBM Wayland interfaces not available
  Failed to initialize glamor, falling back to sw
  ```
  Investigate GPU driver presence, `/dev/dxg`, and the `mesa`/`d3d12` stack if the user reports
  black windows, slow rendering, or missing 3D.

- **Audio sink not connected (usually benign at boot).** Expected until an RDP/audio client attaches:
  ```
  [rdp-sink] RDP Sink - Trying to connect to /mnt/wslg/PulseAudioRDPSink
  [rdp-sink] Connected failed
  ```
  If it persists while audio is actively broken, look at the RDP client (`msrdc`) audio channel.

- **Mostly-benign startup noise** (not a root cause on its own):
  - `WSLGd: Exception: No such file or directory @FontMonitor.cpp:280` - font share not mounted.
  - `XDG_RUNTIME_DIR "/mnt/wslg/runtime-dir" ... mode 040777` (should be 0700) - common warning.
  - `dbus: Unknown username "pulse"` / `Option "-listen" is deprecated` - harmless.

## When to escalate to the WSLg repo

WSLg code is not in this repository. Fixes to weston, FreeRDP, pulseaudio, or the RDP backend
belong in https://github.com/microsoft/wslg. Use a commit-pinned permalink when referencing files
there so the link does not drift, e.g.
`https://github.com/microsoft/wslg/blob/<commit-sha>/<path>`.
