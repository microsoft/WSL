# Wslrelay.exe

`wslrelay.exe` is a windows executable that is used to relay network and debug console traffic from Linux to Windows. 

It is responsible for:

- Relaying localhost traffic between Linux and Windows in NAT mode (see [localhost](localhost.md))
- Displaying the debug console output when `wsl2.debugConsole` is set to `true`

See `src/windows/wslrelay/main.cpp`