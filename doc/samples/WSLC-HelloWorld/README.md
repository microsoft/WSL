# WSLC-HelloWorld

The simplest [WSL Container API](https://aka.ms/wslc) sample, written in **C**
using the flat C API (`wslcsdk.h`). Running `helloworld.exe` starts a lightweight
WSL container from a small Linux image (`alpine:latest`), runs `echo` inside it,
and prints the output to your terminal.

## Build

Open `WSLCHelloWorld.sln` in Visual Studio and build (x64), or from a developer
command prompt:

```
nuget restore WSLCHelloWorld.sln
msbuild WSLCHelloWorld.sln /p:Configuration=Debug /p:Platform=x64
```

## Run

```
x64\Debug\helloworld.exe
```

You should see `Hello, World from a WSL container!` printed to stdout. Progress
messages (`[wslc] ...`) go to stderr.
