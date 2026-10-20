# WSLC-CustomContainer

A barebones C# console sample that turns text (e.g. a URL) into a **scannable QR
code** printed to your terminal — rendered by a tiny Python tool running inside
a **custom Linux container**, using the `Microsoft.WSL.Containers` SDK.

Unlike the other samples (which pull public images), this one ships its own
`Containerfile` that is **built automatically as part of the normal build (F5)**
via the SDK's `<WslcImage>` MSBuild item — no manual `docker`/`wslc` steps. The
app then loads that locally built image from a tar (no registry pull) and runs
`qr.py` inside the container.

## How the auto-build works

`WSLCCustomContainer.csproj` declares:

```xml
<WslcImage Include="customcontainer"
           Image="customcontainer:latest"
           Dockerfile="Container\Containerfile"
           Context="Container"
           Sources="Container"
           TarLocation="$(OutDir)customcontainer.tar" />
```

After `Build`, the package runs `wslc image build` + `wslc image save`,
producing `customcontainer.tar` next to the executable. The step is incremental:
it only re-runs when files under `Container\` change. (Requires the `wslc` CLI,
installed by WSL — `wsl --install --no-distribution`.)

## Build

Requires the .NET 8 SDK. From this folder:

```
dotnet build -c Debug
```

## Run

```
dotnet run -- "https://aka.ms/wslc"
```

Pass any text or URL; with no argument it encodes a sample URL. The QR is drawn
with Unicode blocks so you can scan it straight from the terminal.

## Storage

Everything lives next to the executable (no absolute paths): `WslcQrStorage\`
holds the ephemeral session VHD, and `customcontainer.tar` is the auto-built
image.
