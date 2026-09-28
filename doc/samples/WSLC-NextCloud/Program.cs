// WSLC-NextCloud
//
// A Windows console application that runs a Nextcloud server using the WSL
// Container SDK, written in modern C# with the C#/WinRT projection
// (Microsoft.WSL.Containers).  The container exposes Nextcloud on
// http://localhost:8080 with persistent data stored next to the executable.

using Microsoft.WSL.Containers;

const string imageName = "nextcloud:latest";

// Storage lives beside the executable — no hard-coded absolute paths.  The
// session storage directory must be empty for the session to be created (the
// SDK creates and reuses its own VHD inside it), so the persistent Nextcloud
// data volume lives in a separate sibling directory.
string baseDir = AppContext.BaseDirectory;
string sessionPath = Path.Combine(baseDir, "WslcNextcloudStorage");
string volumePath = Path.Combine(baseDir, "WslcNextcloudData");
Directory.CreateDirectory(volumePath);

int exitCode = 1;
using var stopEvent = new ManualResetEventSlim(false);
var consoleLock = new object();
using Stream stdout = Console.OpenStandardOutput();
using Stream stderr = Console.OpenStandardError();

void Write(Stream target, byte[] data)
{
    lock (consoleLock)
    {
        target.Write(data, 0, data.Length);
        target.Flush();
    }
}

try
{
    // ---- Session ----
    Console.Error.WriteLine("[wslc] Creating session...");
    var sessionSettings = new SessionSettings("WSLCNextCloud", sessionPath)
    {
        CpuCount = 4,
        MemorySizeInMB = 4096,
        // Nextcloud image is ~1.5 GB; use a 10 GB dynamic VHD.
        VhdRequirements = new VhdOptions(string.Empty, 10UL * 1024 * 1024 * 1024, VhdType.Dynamic),
    };

    using var session = new Session(sessionSettings);
    session.Start();

    // ---- Pull image ----
    Console.Error.WriteLine($"[wslc] Pulling image '{imageName}' (this may take several minutes)...");
    session.PullImage(new PullImageOptions(imageName));

    // ---- Create & start container ----
    Console.Error.WriteLine("[wslc] Starting container...");

    // The init process keeps the container alive while we exec the entrypoint.
    var initProcess = new ProcessSettings
    {
        CommandLine = new List<string> { "/bin/sleep", "infinity" },
    };

    var containerSettings = new ContainerSettings(imageName)
    {
        InitProcess = initProcess,
        EnableAutoRemove = true,
        NetworkingMode = ContainerNetworkingMode.Bridged,
        // Port mapping: host 8080 -> container 80.
        PortMappings = new List<ContainerPortMapping> { new(8080, 80, PortProtocol.TCP) },
        // Persistent data volume: bind-mount only the data directory, not the
        // entire webroot.  Mounting /var/www/html over 9P is extremely slow
        // because Nextcloud writes thousands of PHP files there during init.
        Volumes = new List<ContainerVolume> { new(volumePath, "/var/www/html/data", false) },
    };

    using var container = session.CreateContainer(containerSettings);
    container.Start();

    // ---- Exec the Nextcloud entrypoint ----
    Console.Error.WriteLine("[wslc] Launching Nextcloud entrypoint...");
    var processSettings = new ProcessSettings
    {
        CommandLine = new List<string> { "/entrypoint.sh", "apache2-foreground" },
        OutputMode = ProcessOutputMode.Event,
    };

    using var process = container.CreateProcess(processSettings);
    process.OutputReceived += data => Write(stdout, data);
    process.ErrorReceived += data => Write(stderr, data);
    process.Exited += code =>
    {
        exitCode = code;
        stopEvent.Set();
    };

    process.Start();

    Console.Error.WriteLine();
    Console.Error.WriteLine("[wslc] Nextcloud is running at http://localhost:8080");
    Console.Error.WriteLine("[wslc] Press Enter to stop...");
    Console.Error.WriteLine();

    // Stop when the user presses Enter (or the entrypoint exits on its own).
    var inputThread = new Thread(() =>
    {
        Console.ReadLine();
        if (!stopEvent.IsSet)
        {
            exitCode = 0;
            stopEvent.Set();
        }
    })
    { IsBackground = true };
    inputThread.Start();

    stopEvent.Wait();

    Console.Error.WriteLine("[wslc] Shutting down...");
    container.Stop(Signal.SIGTERM, TimeSpan.FromSeconds(10));
    session.Terminate();
    Console.Error.WriteLine("[wslc] Done.");
}
catch (Exception ex)
{
    Console.Error.WriteLine($"[wslc] Error: {ex.Message}");
}

return exitCode;
