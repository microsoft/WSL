Please provide as much information as possible when reporting a bug or filing an issue on the Windows Subsystem for Linux.

## Important: Reporting BSODs and Security issues
**Do not open GitHub issues for Windows crashes (BSODs) or security issues.**. Instead, send Windows crashes or other security-related issues to secure@microsoft.com.
See the `10) Reporting a Windows crash (BSOD)` section below for detailed instructions.

## Reporting issues in Windows Console or WSL text rendering/user experience
Note that WSL distro's launch in the Windows Console (unless you have taken steps to launch a 3rd party console/terminal). Therefore, *please file UI/UX related issues in the [Windows Console issue tracker](https://github.com/microsoft/console)*.

## Reporting issues in WSL
A well written bug will follow the following template:

### 1) Issue Title
A title succinctly describing the issue.

#### Example:
`Traceroute not working.`

### 2) Windows version / build number
Your Windows build number.  This can be gathered from the CMD prompt using the `cmd.exe --version` command.

```cmd.exe
C:\ cmd.exe --version
Microsoft Windows [Version 10.0.21354.1]
```

Note: The Windows Insider builds contain many updates and fixes. If you are running on the Creators Update (10.0.15063) please check to see if your issue has been resolved in a later build.  If you are running on a Version below (10.0.14393), please try to update your Version.

#### Example:

`Microsoft Windows [Version 10.0.21354.1]`

### 3) Steps required to reproduce

Should include all packages and environmental variables as well as other required configuration.

#### Example: On linux 

`$ sudo apt-get install traceroute && traceroute www.microsoft.com`

#### Example: On Windows

``
`$ cmd.exe` 
`CD C:\Windows\System32\`
`tracert.exe`
``

### 4) Copy of the terminal output

#### Example:

```
$ traceroute www.microsoft.com
traceroute to www.microsoft.com (23.75.239.28), 30 hops max, 60 byte packets
setsockopt IP_MTU_DISCOVER: Invalid argument
```

### 5) Expected Behavior

What was the expected result of the command?  Include examples / documentation if possible.

### 6) Strace of the failing command

Run the failing command under [strace](http://manpages.ubuntu.com/manpages/wily/man1/strace.1.html).  Normal command structure is:

```
$ strace -ff <command>
```

> Note: `strace` can produce lengthy output. If the generated trace is more than about 20 lines please paste this into a [Gist](https://gist.github.com/) or another paste service and link in the bug.

#### Example:

```
$ strace traceroute www.microsoft.com
execve("/usr/bin/traceroute", ["traceroute", "www.microsoft.com"], [/* 22 vars */]) = 0
brk(0)                                  = 0x7fffdd3bc000
access("/etc/ld.so.nohwcap", F_OK)      = -1 ENOENT (No such file or directory)
mmap(NULL, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x7f1f4e820000
access("/etc/ld.so.preload", R_OK)      = -1 ENOENT (No such file or directory)
...
...
...
```

### 7) Additional information

Some bugs require additional information such as scripts to reproduce.  Please add to this section.

If there are files required, email the files to InsiderSupport@microsoft.com with:

* **Subject**:  Forward to WSL Team - RE: github issue <issue #>
* **Body**:  "Please forward to WSL Team" and include your attachment.

Common files are:

* Memory dumps found under C:\Windows\MEMORY.DMP
* Additional strace logs if the error occurs within a fork. The following
  command generates an output file for every fork created:

```
$ strace -ff -o <outputfile> <command>
```

#### wsl --mount

If the issue is about wsl --mount, please include the output of running `wmic diskdrive get Availability,Capabilities,CapabilityDescriptions,DeviceID,InterfaceType,MediaLoaded,MediaType,Model,Name,Partitions` in an elevated command prompt.

Example:

```
C:\WINDOWS\system32>wmic diskdrive get Availability,Capabilities,CapabilityDescriptions,DeviceID,InterfaceType,MediaLoaded,MediaType,Model,Name,Partitions
Availability  Capabilities  CapabilityDescriptions                                       DeviceID            InterfaceType  MediaLoaded  MediaType              Model                       Name                Partitions
              {3, 4}        {"Random Access", "Supports Writing"}                        \\.\PHYSICALDRIVE0  SCSI           TRUE         Fixed hard disk media  SAMSUNG MZVLB512HAJQ-000H2  \\.\PHYSICALDRIVE0  3
              {3, 4}        {"Random Access", "Supports Writing"}                        \\.\PHYSICALDRIVE1  SCSI           TRUE         Fixed hard disk media  SAMSUNG MZVLB1T0HALR-000H2  \\.\PHYSICALDRIVE1  1
              {3, 4, 10}    {"Random Access", "Supports Writing", "SMART Notification"}  \\.\PHYSICALDRIVE2  SCSI           TRUE         Fixed hard disk media  ST2000DM001-1ER164          \\.\PHYSICALDRIVE2  1
```

#### Networking issues

Install tcpdump in your WSL distribution using the following commands.
Note: This might not work if WSL has Internet connectivity issues.

```
# sudo apt-get update
# sudo apt-get -y install tcpdump
```

Run networking_diagnostics.bat in an administrative Powershell window:

```
$ Invoke-WebRequest 'https://github.com/microsoft/WSL/archive/refs/heads/master.zip' -OutFile .\wsl.zip
$ Expand-Archive .\wsl.zip .\
$ Remove-Item .\wsl.zip
$ cd .\WSL-master\diagnostics
$ .\networking_diagnostics.bat
```

Note:
If tcpdump is installed, the script will open 2 more shells.
If tcpdump is not installed, only 1 additional shell will be opened
Wait for those shells to be opened before reproducing the issue

After reproducing the issue, in the shell with tcpdump, hit Ctrl + C. In the other shell, press any key.
After the new shells exit, press any key in the original Powershell window.

The script will output the path to a zip archive with the diagnostics when done. Collect the zip and attach it to the issue.

<!-- Preserving anchors -->
<div id="8-detailed-logs"></div>
<div id="9-networking-logs"></div>


### 8) Collect WSL logs (recommended method)

To collect WSL logs, download and execute [collect-wsl-logs.ps1](https://github.com/Microsoft/WSL/blob/master/diagnostics/collect-wsl-logs.ps1) in an administrative powershell prompt:

```
Invoke-WebRequest -UseBasicParsing "https://raw.githubusercontent.com/microsoft/WSL/master/diagnostics/collect-wsl-logs.ps1" -OutFile collect-wsl-logs.ps1
Set-ExecutionPolicy Bypass -Scope Process -Force
.\collect-wsl-logs.ps1
```
The script will output the path of the log file once done.

### 9) Collect WSL logs with Feedback hub
To collect WSL logs follow these steps: 

#### Open Feedback Hub and enter the title and description of your issue

- Open Feedback hub and create a new issue by pressing `Windows Key + F` on your keyboard. 
- Enter in the details of your issue:
   - In `Summarize your feedback` copy and paste in the title of your Github Issue
   - In `Explain in more detail` copy and paste a link to your Github Issue

![GIF Of networking instructions](img/networkinglog1.gif)

#### Choose the WSL category 

- Select that your issue is a `Problem`
- Choose the `Developer Platform` category and the `Windows Subsystem for Linux` subcategory

![GIF Of networking instructions](img/networkinglog2.gif)

#### Recreate your problem in the 'Additional Details' section

- Select 'Other' under 'Which of the following best describes your problem'
- Click 'Recreate My Problem' under 'Attachments
- Ensure that `Include Data About:` is checked to 'Windows Subsystem for Linux' 
- 'Click Start Recording' to start collecting logs
- Recreate your problem
- Click 'Stop Recording'

![GIF Of networking instructions](img/networkinglog3.gif)

#### Check your attachments and submit

- Verify your recording is attached and whether you would like to send the screenshot that is automatically attached
- Hit Submit
- Get a link to your feedback item by clicking on 'Share my Feedback' and post that link to the Github thread so we can easily get to your feedback!

![GIF Of networking instructions](img/networkinglog4.gif)


### 10) Reporting a Windows crash (BSOD)

To collect a kernel crash dump, first run the following command in an elevated command prompt:

```
reg.exe add HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\CrashControl /v AlwaysKeepMemoryDump  /t REG_DWORD /d 1 /f
```

Then reproduce the issue, and let the machine crash and reboot.

After reboot, the kernel dump will be in `%SystemRoot%\MEMORY.DMP` (unless this path has been overriden in the advanced system settings).

Please send this dump to: secure@microsoft.com .
Make sure that the email body contains:

- The Github issue number, if any
- That this dump is destinated to the WSL team

### 11) Reporting a WSL process crash

The easiest way to report a WSL process crash is by [collecting a user-mode crash dump](https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps).

To enable automatic crash dumps, run the following commands in an elevated command prompt:

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

1) [Install Windbg preview](https://apps.microsoft.com/store/detail/windbg-preview/9PGJGD53TN86?hl=en-us&gl=us&rtc=1)

2) Open windbg preview as administrator by running `windbgx` in an elevated command prompt

3) Navigate to `file` -> `Attach to process`

4) Check `Record with Time Travel Debugging` (at the bottom right)

4) Check `Show processes from all users` (at the bottom)

5) Select `wslservice.exe`. Note, if wslservice.exe is not running, you make it start it with: `wsl.exe -l`

6) Click `Configure and Record` (write down the folder you chose for the traces)

7) Reproduce the issue

8) Go back to windbg and click `Stop and Debug`

9) Once the trace is done collecting, click `Stop Debugging` and close Windbg

10) Go to the folder where the trace was colleced, and locate the .run file. It should look like: `wslservice*.run`

11) Share that file on the issue