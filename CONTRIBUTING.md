When reporting a bug, please be sure post the following information:

1. The build of Windows you're running. Example:
```cmd
C:\Users\Dave>ver

Microsoft Windows [Version 10.0.14385]
```

2. The exact command you tried to run. Example:
```cmd
dave@remoteoffice2.nsa.gov:~$ ps aux
Error: /proc must be mounted
  To mount /proc at boot you need an /etc/fstab line like:
      proc   /proc   proc    defaults
  In the meantime, run "mount proc /proc -t proc"
```

3. The strace log of the command that failed to run. Please paste this into [Gist](https://gist.github.com/) or another paste service. Example:
```cmd
dave@remoteoffice2.nsa.gov:~$ strace whoami
execve("/usr/bin/whoami", ["whoami"], [/* 16 vars */]) = 0
brk(NULL)                               = 0x2423000
access("/etc/ld.so.nohwcap", F_OK)      = -1 ENOENT (No such file or directory)
.... this file will be very large ....
close(2)                                = 0
exit_group(0)                           = ?
+++ exited with 0 +++
```
