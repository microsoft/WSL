# wsl.exe

wsl.exe is the main command line entrypoint for WSL. Its job is to:

- Parse the command line arguments (See `src/windows/common/wslclient.cpp`)
- Call [wslservice.exe](wslservice.exe.md) via COM to launch WSL (see `src/windows/common/svccomm.cpp`)
- Relay stdin / stdout / stderr from and to the linux process

