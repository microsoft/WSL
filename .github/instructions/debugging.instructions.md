---
description: 'WSL debugging, ETL tracing, log analysis, and debugger attachment'
applyTo: 'diagnostics/**'
---

# Debugging — WSL

Reference: https://wsl.dev/debugging/

## ETL Tracing (Windows)

```powershell
wpr -start diagnostics\wsl.wprp -filemode
# [reproduce the issue]
wpr -stop logs.ETL
```

### Trace Profiles

Append `!ProfileName` to the wprp path:

| Profile | Purpose |
|---------|---------|
| `WSL` (default) | General WSL tracing |
| `WSL-Storage` | Enhanced storage tracing |
| `WSL-Networking` | Comprehensive networking tracing |
| `WSL-HvSocket` | HvSocket-specific tracing |

Example: `wpr -start diagnostics\wsl.wprp!WSL-Networking -filemode`

### Notable ETL Providers

**`Microsoft.Windows.Lxss.Manager`** (wslservice.exe):
- `GuestLog` — VM dmesg logs
- `Error` — Unexpected errors
- `CreateVmBegin` / `CreateVmEnd` — Virtual machine lifetime
- `CreateNetworkBegin` / `CreateNetworkEnd` — Networking configuration
- `SentMessage` / `ReceivedMessage` — Hvsocket channel communication with Linux

**`Microsoft.Windows.Subsystem.Lxss`** (wsl.exe, wslg.exe, wslconfig.exe, wslrelay.exe):
- `UserVisibleError` — An error displayed to the user

**`Microsoft.Windows.Plan9.Server`** — Plan9 server logs (for `/mnt/` shares and Windows interop)

### Log Analysis

- View ETL traces with [WPA (Windows Performance Analyzer)](https://apps.microsoft.com/detail/9n58qrw40dfw)
- Collect WSL logs: `powershell diagnostics\collect-wsl-logs.ps1`
- Network logs: `powershell diagnostics\collect-wsl-logs.ps1 -LogProfile networking`

## Attaching Debuggers

### Windows Processes
- Attach usermode debuggers to wsl.exe, wslservice.exe, wslrelay.exe, etc.
- Symbols available under `bin\<platform>\<target>\`
- For automatic crash dump collection, see `CONTRIBUTING.md` (section on reporting process crashes)

### Linux Processes
- Use `gdb` to attach to Linux processes
- Access source from gdb via `/mnt` mountpoints: `dir /path/to/wsl/source`

### Root Namespace (gns, mini_init)
These processes aren't accessible from within WSL distributions. Use the debug shell:
```bash
wsl --debug-shell
tdnf install gdb
# Now attach gdb to gns, mini_init, etc.
```

## Debug Console

Enable Linux debug console output by adding to `%USERPROFILE%\.wslconfig`:
```ini
[wsl2]
debugConsole=true
```
Then restart WSL. Output is relayed by `wslrelay.exe`.
