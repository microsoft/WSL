# WSL contributing guide

There are a few main ways to contribute to WSL, with guides to each one:

1. [Add a feature or bugfix to WSL](#add-a-feature-or-bugfix-to-wsl)
2. [File a WSL issue](#file-a-wsl-issue)

## Add a feature or bugfix to WSL

We welcome any contributions to the WSL source code to add features or fix bugs! Before you start actually working on the feature, please **[file it as an issue, or a feature request in this repository](https://github.com/microsoft/WSL/issues)** so that we can track it and provide any feedback if necessary.

Once you have done so, please see [the developer docs](./doc/docs/dev-loop.md) for instructions on how to build WSL locally on your machine for development. 

When your fix is ready, please [submit it as a pull request in this repository](https://github.com/microsoft/WSL/pulls) and the WSL team will triage and respond to it. Most contributions require you to agree to a Contributor License Agreement (CLA) declaring that you have the right to, and actually do, grant us the rights to use your contribution. For details, visit https://cla.microsoft.com.

## File a WSL issue

You can file issues for WSL at the WSL repository, or linked repositories. Before filing an issue please search for any existing issues and upvote or comment on those if possible. 

1. If your issue is related to WSL documentation, please file it at [microsoftdocs/wsl](https://github.com/microsoftdocs/WSL/issues)
2. If your issue is related to a Linux GUI app, please file it at [microsoft/wslg](https://github.com/microsoft/wslg/issues)
3. Otherwise, if you have a technical issue related to WSL in general, such as start up issues, etc., please file it at [microsoft/wsl](https://github.com/microsoft/WSL/issues)

Please provide as much information as possible when reporting a bug or filing an issue on the Windows Subsystem for Linux, and be sure to include logs as necessary!

Please see the [notes for collecting WSL logs](#notes-for-collecting-wsl-logs) section below for more info on filing issues.

## Thank you

Thank you in advance for your contribution! We appreciate your help in making WSL a better tool for everyone.

## Notes for collecting WSL logs

### Important: Reporting BSODs and Security issues
**Do not open GitHub issues for Windows crashes (BSODs) or security issues.**. Instead, send Windows crashes or other security-related issues to secure@microsoft.com.
See the `10) Reporting a Windows crash (BSOD)` section below for detailed instructions.

### Reporting issues in Windows Console or WSL text rendering/user experience
Note that WSL distro's launch in the Windows Console (unless you have taken steps to launch a 3rd party console/terminal). Therefore, *please file UI/UX related issues in the [Windows Console issue tracker](https://github.com/microsoft/console)*.

### Collect WSL logs for networking issues

Install tcpdump in your WSL distribution using the following commands.
Note: This will not work if WSL has Internet connectivity issues.

```
# sudo apt-get update
# sudo apt-get -y install tcpdump
```

Install [WPR](https://learn.microsoft.com/windows-hardware/test/wpt/windows-performance-recorder)

To collect WSL networking logs, do the following steps in an administrative powershell prompt:

```
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/collect-wsl-logs.ps1" -OutFile collect-wsl-logs.ps1
Set-ExecutionPolicy Bypass -Scope Process -Force
.\collect-wsl-logs.ps1 -LogProfile networking
```
The script will output when log collection starts. Reproduce the problem, then press any key to stop the log collection.
The script will output the path of the log file once done.

For additional network creation logs (restarts WSL), use:
```
.\collect-wsl-logs.ps1 -LogProfile networking -RestartWslReproMode
```

<!-- Preserving anchors -->
<div id="8-detailed-logs"></div>
<div id="9-networking-logs"></div>
<div id="8-collect-wsl-logs-recommended-method"></div>


### Collect WSL logs (recommended method)

If you choose to email these logs instead of attaching them to the bug, please send them to wsl-gh-logs@microsoft.com with the GitHub issue number in the subject, and include a link to your GitHub issue comment in the message body.

To collect WSL logs, download and execute [collect-wsl-logs.ps1](https://github.com/Microsoft/WSL/blob/master/diagnostics/collect-wsl-logs.ps1) in an administrative powershell prompt:

```
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/collect-wsl-logs.ps1" -OutFile collect-wsl-logs.ps1
Set-ExecutionPolicy Bypass -Scope Process -Force
.\collect-wsl-logs.ps1
```
The script will output the path of the log file once done.

For specific scenarios, you can use different log profiles:
- `.\collect-wsl-logs.ps1 -LogProfile storage` - Enhanced storage tracing
- `.\collect-wsl-logs.ps1 -LogProfile networking` - Comprehensive networking tracing (includes packet capture, tcpdump, etc.)
- `.\collect-wsl-logs.ps1 -LogProfile networking -RestartWslReproMode` - Networking tracing with WSL restart for network creation logs
- `.\collect-wsl-logs.ps1 -LogProfile hvsocket` - HvSocket-specific tracing

### 10) Reporting a Windows crash (BSOD)

To collect a kernel crash dump, first run the following command in an elevated command prompt:

```
reg.exe add HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\CrashControl /v AlwaysKeepMemoryDump  /t REG_DWORD /d 1 /f
```

Then reproduce the issue, and let the machine crash and reboot.

After reboot, the kernel dump will be in `%SystemRoot%\MEMORY.DMP` (unless this path has been overridden in the advanced system settings).

Please send this dump to: secure@microsoft.com .
Make sure that the email body contains:

- The GitHub issue number, if any
- That this dump is intended for the WSL team

### 11) Reporting a WSL process crash

The easiest way to report a WSL process crash is by [collecting a user-mode crash dump](https://learn.microsoft.com/windows/win32/wer/collecting-user-mode-dumps).

To collect dumps of all running WSL processes, please open a PowerShell prompt with admin privileges, navigate to a folder where you'd like to put your log files and run these commands: 

```
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/collect-wsl-logs.ps1" -OutFile collect-wsl-logs.ps1
Set-ExecutionPolicy Bypass -Scope Process -Force
.\collect-wsl-logs.ps1 -Dump
```

The script will output the path to the log file when it is done.

#### Enable automatic crash dump collection

If your crash is sporadic or hard to reproduce, please enable automatic crash dumps to catch logs for this behavior: 

```
md C:\crashes
reg.exe add "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /f
reg.exe add "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /v DumpFolder /t REG_EXPAND_SZ /d C:\crashes /f
reg.exe add "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /v DumpType /t REG_DWORD /d 2 /f
```

Crash dumps will then automatically be written to C:\crashes.

Once you're done, crash dump collection can be disabled by running the following command in an elevated command prompt:

```
reg.exe delete "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /f
```

### 12) Collect wslservice time travel debugging traces

To collect time travel debugging traces:

1) [Install WinDbg preview](https://apps.microsoft.com/store/detail/windbg-preview/9PGJGD53TN86?hl=en-us&gl=us&rtc=1)

2) Open WinDbg preview as administrator by running `windbgx` in an elevated command prompt

3) Navigate to `file` -> `Attach to process`

4) Check `Record with Time Travel Debugging` (at the bottom right)

4) Check `Show processes from all users` (at the bottom)

5) Select `wslservice.exe`. Note, if wslservice.exe is not running, you make it start it with: `wsl.exe -l`

6) Click `Configure and Record` (write down the folder you chose for the traces)

7) Reproduce the issue

8) Go back to WinDbg and click `Stop and Debug`

9) Once the trace is done collecting, click `Stop Debugging` and close WinDbg

10) Go to the folder where the trace was collected, and locate the .run file. It should look like: `wslservice*.run`

11) Share that file on the issue
