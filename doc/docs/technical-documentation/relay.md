# Relay

Relay is a WSL2 linux process created by a [session leader](session-leader.md). Its job is to create a linux process on behalf of the user, and relay its output back to Windows. 

## Creating a user process

A relay is created when a `LxInitMessageCreateProcessUtilityVm` message is sent to a [session leader](session-leader.md). Once created, the `relay` creates multiple `hvsocket` channels with [wslservice.exe](wslservice.exe.md).

These channels are used to:

- Relay standard file descriptors (stdin, stdout, stderr)
- Relay information about the terminal (for instance when the terminal window is resized from Windows)
- Notify Windows when the linux process exits

Once those channels are configured, the `relay` forks() into two processes: 

- The parent, which will read & write to the child's standard file descriptors and relay it to Windows
- The child, which calls `exec()` and starts the user process