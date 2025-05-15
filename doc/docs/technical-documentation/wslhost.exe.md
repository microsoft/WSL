# Wslhost.exe 

`wslhost.exe` is a Windows executable that's used to display desktop notifications, and run Linux processes in the background.

## COM server

When running as COM server, `wslhost.exe` registers a [NotificationActivatorFactory](https://learn.microsoft.com/dotnet/api/microsoft.toolkit.uwp.notifications.notificationactivator?view=win-comm-toolkit-dotnet-7.1), which is then used to display desktop notifications to the user.

Notifications can be used to:

- Notify the user about a WSL update
- Warn the user about a configuration error
- Notify the user about a proxy change

See: `src/windows/common/notifications.cpp`

## Background processes 

When [wsl.exe](wsl.exe.md) terminates before the associated Linux processes terminates, `wslhost.exe` takes over the lifetime of the Linux process. 

This allows Linux processes to keep running Windows commands and access the terminal even after the associated `wsl.exe` terminates. 

See `src/windows/wslhost/main.cpp` and [interop](interop.md)