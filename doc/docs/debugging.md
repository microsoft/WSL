# Debugging WSL

## Logging

There are multiple sources of logging in WSL. The main one is the ETL trace that is emitted from Windows processes.

To collect an ETL trace, run ([link to wsl.wprp](https://github.com/microsoft/WSL/blob/master/diagnostics/wsl.wprp)):

```
wpr -start wsl.wprp -filemode

[reproduce the issue]

wpr -stop logs.ETL
```

The consolidated `wsl.wprp` file includes multiple profiles for different scenarios:
- `WSL` - General WSL tracing (default)
- `WSL-Storage` - Enhanced storage tracing
- `WSL-Networking` - Comprehensive networking tracing
- `WSL-HvSocket` - HvSocket-specific tracing

To use a specific profile, append `!ProfileName` to the wprp file, e.g., `wpr -start wsl.wprp!WSL-Networking -filemode`

Once the log file is saved, you can use [WPA](https://apps.microsoft.com/detail/9n58qrw40dfw?hl=en-US&gl=US) to view the logs.

Notable ETL providers: 

- `Microsoft.Windows.Lxss.Manager`: Logs emitted from wslservice.exe
    Important events: 
    - `GuestLog`: Logs from the vm's dmesg
    - `Error`: Unexpected errors
    - `CreateVmBegin`, `CreateVmEnd`: Virtual machine lifetime
    - `CreateNetworkBegin`, `CreateNetworkEnd`: Networking configuration
    - `SentMessage`, `ReceivedMessage`: Communication on the hvsocket channels with Linux.
    
- `Microsoft.Windows.Subsystem.Lxss`: Other WSL executables (wsl.exe, wslg.exe, wslconfig.exe, wslrelay.exe, ...)
    Important events:
    - `UserVisibleError`: An error was displayed to the user 

- `Microsoft.Windows.Plan9.Server`: Logs from the Windows plan9 server (used when accessing /mnt/ shares and running Windows)


On the Linux side, the easiest way to access logs is to look at `dmesg` or use the debug console, which can enabled by writing:

```
[wsl2]
debugConsole=true
```

to `%USERPROFILE%/.wslconfig` and restarting WSL


## Attaching debuggers

Usermode can be attached to WSL Windows processes (wsl.exe, wslservice.exe, wslrelay.exe, ...). The symbols are available under the `bin/<platform>/<target>` folder. 
You can also use [this trick](https://github.com/microsoft/WSL/blob/master/CONTRIBUTING.md#11-reporting-a-wsl-process-crash) to automatically collect crash dumps when processes crash.

## Linux debugging

`gdb` can be attached to Linux processes (see [man gdb](https://man7.org/linux/man-pages/man1/gdb.1.html)). 

The simplest way to debug a WSL process with gdb is to use the `/mnt` mountpoints to access the code from gdb. 
Once started, just use `dir /path/to/wsl/source` in gdb to connect the source files.

## Root namespace debugging

Some WSL process such as `gns` or `mini_init` aren't accessible from within WSL distributions. To attach a debugger to those, use the debug shell via:

```
wsl --debug-shell
```

You can then install `gdb` by running `tdnf install gdb` and start debugging processes.