Please provide as much information as possible when reporting a bug or filing an issue on the Windows Subsystem for Linux.

## Important: Reporting BSODs and Security issues
**Do not open Github issues for Windows crashes (BSODs) or security issues.**. Instead, send Windows crashes or other security-related issues to secure@microsoft.com.

## Reporting issues in Windows Console or WSL text rendering/user experience
Note that WSL distro's launch in the Windows Console (unless you have taken steps to launch a 3rd party console/terminal). Therefore, *please file UI/UX related issues in the [Windows Console issue tracker](https://github.com/microsoft/console)*.

## Reporting issues in WSL
A well written bug will follow the following template:

### 1) Issue Title
A title succinctly describing the issue.

#### Example:
`Traceroute not working.`

### 2) Windows version / build number
Your Windows build number.  This can be gathered from the CMD prompt using the `ver` command.

```
C:\> ver
Microsoft Windows [Version 10.0.14385]
```

Note: The Windows Insider builds contain many updates and fixes. If you are running on the Creators Update (10.0.15063) please check to see if your issue has been resolved in a later build.  If you are running on the Anniversary Update (10.0.14393), please try updating to the Creators Update.

#### Example:

`Microsoft Windows [Version 10.0.16170]`

### 3) Steps required to reproduce

Should include all packages and environmental variables as well as other required configuration.

#### Example:

`$ sudo apt-get install traceroute && traceroute www.microsoft.com`

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

If the issue is about networking, run [networking.bat](https://github.com/Microsoft/WSL/blob/master/diagnostics/networking.bat) in an administrative command prompt:

```
$ git clone https://github.com/microsoft/WSL --depth=1 %tmp%\WSL
$ cd %tmp%\WSL\diagnostics
$ networking.bat
```

Once the script execution is completed, include **both** its output and the generated log file, `wsl.etl` on the issue.

<!-- Preserving anchors -->
<div id="8-detailed-logs"></div>
<div id="9-networking-logs"></div>

### 8) Collect WSL logs
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

#### Record WSL logs manually

If creating a Feedback Hub entry isn't possible, then WSL logs need to be captured manually.

To do so, first download [wsl.wprp](https://github.com/microsoft/WSL/blob/master/diagnostics/wsl.wprp), then run in an administrative command prompt:

```
$ wpr -start wsl.wprp -filemode
```

Once the log collection is started, reproduce the problem, and run:

```
$ wpr -stop wsl.etl
```

This will stop the log collection and create a file named `wsl.etl` that will capture diagnostics information.