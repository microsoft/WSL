Please provide as much information as possible when reporting a bug or filing an issue on the Windows Subsystem for Linux.

Do not open Github issues for Windows crashes (BSODs) or security issues.  Please direct all Windows crashes and security issues to secure@microsoft.com.  Issues with security vulnerabilities may be edited to hide the vulnerability details.

A well written bug will follow the template:

### 1) Issue Title

A title succinctly describing the issue. 

#### Example:

`Traceroute not working.`


### 2) Brief description

A brief description of what you are attempting to run.

####Example:

`IP_MTU_DISCOVER error when running traceroute.`

### 3) Windows version / build number

Your Windows build number.  This can be gathered from the CMD prompt using the `ver` command.

```
C:\>ver 
Microsoft Windows [Version 10.0.14385] 
``` 

Note: The Windows Insider builds contain many updates and fixes. If you are running on the Anniversary Update (10.0.14393) please check to see if your issue has been resolved in a later build.

#### Example:

`Microsoft Windows [Version 10.0.14385]`

### 4) Steps required to reproduce

Should include all packages and environmental variables as well as other required configuration.

#### Example:

`$ sudo apt-get install traceroute`
`$ traceroute www.microsoft.com`

### 5) Copy of the terminal output

#### Example:

```
russ@RUSSALEX-BOOK:/mnt/c$ traceroute www.microsoft.com
traceroute to www.microsoft.com (23.75.239.28), 30 hops max, 60 byte packets
setsockopt IP_MTU_DISCOVER: Invalid argument
```

### 6) Expected Behavior

What was the expected result of the command?  Include examples / documentation if possible.

### 7) Strace of the failing command

Run the failing command under [strace] (http://manpages.ubuntu.com/manpages/wily/man1/strace.1.html).  Normal command structure is:

```                           
$ strace <command> 
```          

Strace can produce a very long output.  If this is more than about 20 lines please paste this into [Gist](https://gist.github.com/) or another paste service and link in the bug.

#### Example:

```
russ@RUSSALEX-BOOK:/mnt/c$ strace traceroute www.microsoft.com
execve("/usr/bin/traceroute", ["traceroute", "www.microsoft.com"], [/* 22 vars */]) = 0
brk(0)                                  = 0x7fffdd3bc000
access("/etc/ld.so.nohwcap", F_OK)      = -1 ENOENT (No such file or directory)
mmap(NULL, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x7f1f4e820000
access("/etc/ld.so.preload", R_OK)      = -1 ENOENT (No such file or directory)
...
...
...
```

### 8) Additional information

Some bugs require additional information such as scripts to reproduce.  Please add to this section.

If there are files required, email the files to InsiderSupport@microsoft.com with:
Subject:  Forward to WSL Team - RE: github issue <issue #>
Body:  "Please forward to WSL Team" and include your attachment.

Common files are:
Memory dumps found under C:\Windows\MEMORY.DMP
Additional strace logs if the error occurs within a fork.  The following command generates an output file for every fork created:

``` 
$ strace -ff -o <outputfile> <command> 
```

### 9) Detailed Logs
Some bugs will require more detailed logs to help determine the cause.  There is a CMD command to start detailed logging and another to stop.  The logs are generated locally into the working directory.

#### Start

``` 
>logman.exe create trace lxcore_kernel -p {0CD1C309-0878-4515-83DB-749843B3F5C9} -mode 0x00000008 -ft 10:00 -o .\lxcore_kernel.etl -ets 
>logman create trace lxcore_adss -p {754E4536-6735-4194-BE81-1374BD2E9B0D} -ft 1:00 -rt -o .\lxcore_adss.etl -ets 
>logman create trace lxcore_user -p {D90B9468-67F0-5B3B-42CC-82AC81FFD960} -ft 1:00 -rt -o .\lxcore_user.etl -ets 
>logman create trace lxcore_service -p {B99CDB5A-039C-5046-E672-1A0DE0A40211} -ft 1:00 -rt -o .\lxcore_service.etl -ets 
``` 

#### Stop 

``` 
>logman stop lxcore_kernel -ets
>logman stop lxcore_adss -ets
>logman stop lxcore_user -ets
>logman stop lxcore_service -ets
``` 

#### Output

Files generated are in the directory where the commands above ran:

```          
lxcore_adss.etl
lxcore_kernel.etl
lxcore_service.etl
lxcore_user.etl
```
