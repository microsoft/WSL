# WSLC-Neofetch

A minimal sample that uses the [WSL Container API](https://aka.ms/wslc) to run the
Linux `neofetch` command from a native Windows executable. Running `neofetch.exe`
starts a lightweight WSL container, runs `neofetch` inside it, and streams the
output back to your terminal. Command-line arguments are forwarded, so
`neofetch.exe --help` works just like `neofetch --help` on Linux.

## Build

Open `WSLCNeofetch.sln` in Visual Studio and build (x64), or from a developer
command prompt:

```
nuget restore WSLCNeofetch.sln
msbuild WSLCNeofetch.sln /p:Configuration=Debug /p:Platform=x64
```

## Run

```
x64\Debug\neofetch.exe            # show system info
x64\Debug\neofetch.exe --help     # forwarded to neofetch
```

Progress messages (`[wslc] ...`) are written to stderr, so piping stdout is safe.
