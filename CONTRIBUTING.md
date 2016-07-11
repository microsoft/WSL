Please provide as much information as possible when reporting a bug or filing an issue on the Windows Subsystem for Linux.
A well written bug will follow the template:

1) Issue Title

2) Brief description
Briefly describe what you are attemping to run.

3) Windows version / build number
Your Windows build number.  This can be gathered from the CMD prompt using the `ver` command.

```
C:\>ver 
Microsoft Windows [Version 10.0.14385] 
``` 

4) Commands required to reproduce the error
Should include all packages required to install and configuration required.

5) Copy of the terminal output

6) Strace of the failing command
Run the failing command under [strace] (http://manpages.ubuntu.com/manpages/wily/man1/strace.1.html).  Normal command structure is:

```                           
$ strace <command> 
```          

Strace can produce a very long output.  If this is more than about 20 lines please paste this into [Gist](https://gist.github.com/) or another paste service and link in the bug.

7) Additional information
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


-- Example Bug

1)`Traceroute not working.`
2)`IP_MTU_DISCOVER error when running traceroute.`

3)`Microsoft Windows [Version 10.0.14385]`

4)`$ sudo apt-get install traceroute`
`$ traceroute www.microsoft.com`

5) Copy of the terminal output

```russ@RUSSALEX-BOOK:/mnt/c$ traceroute www.microsoft.com
traceroute to www.microsoft.com (23.75.239.28), 30 hops max, 60 byte packets
setsockopt IP_MTU_DISCOVER: Invalid argument```

6)
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
