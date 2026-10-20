# WSLC-NextCloud

A sample that uses the [WSL Container API](https://aka.ms/wslc) to run a
**Nextcloud** server from a native Windows executable, written in modern C# with
the `Microsoft.WSL.Containers` C#/WinRT projection. Running `nextcloud.exe`
starts a lightweight WSL container, pulls the official `nextcloud` image, and
serves it on **http://localhost:8080** (host port 8080 → container port 80).

## Build

Requires the .NET 8 SDK. From this folder:

```
dotnet build -c Debug
```

## Run

```
dotnet run -c Debug
```

Open **http://localhost:8080** in your browser, then press **Enter** in the
terminal to stop the server and clean up. The first run pulls a ~1.5 GB image
and may take several minutes.

## Storage

Two folders are created next to the executable (no absolute paths):
`WslcNextcloudStorage\` holds the ephemeral session VHD, and `WslcNextcloudData\`
is bind-mounted at `/var/www/html/data` so user data persists between runs.
