// WSLC-CustomContainer
//
// A barebones Windows console application that turns text (e.g. a URL) into a
// scannable QR code, rendered by a tiny Python tool running inside a *custom*
// Linux container.
//
// What makes this sample different from the others: it ships its own
// Containerfile that is built automatically as part of the normal build (F5)
// via the <WslcImage> item in WSLCCustomContainer.csproj. The built image is
// saved to customcontainer.tar next to the executable, and this app loads that
// local tar (no registry pull) before running the tool.
//
//   dotnet run -- "https://aka.ms/wslc"

using Microsoft.WSL.Containers;

const string imageName = "customcontainer:latest";

// The text to encode comes from the command line; fall back to a sample URL.
string text = args.Length > 0 ? string.Join(' ', args) : "https://aka.ms/wslc";

// Everything lives beside the executable — no hard-coded absolute paths. The
// container image tar is produced next to the exe by the build.
string baseDir = AppContext.BaseDirectory;
string sessionPath = Path.Combine(baseDir, "WslcQrStorage");
string imageTarPath = Path.Combine(baseDir, "customcontainer.tar");

if (!File.Exists(imageTarPath))
{
    Console.Error.WriteLine($"[wslc] Image tar not found: {imageTarPath}");
    Console.Error.WriteLine("[wslc] Build the project first so the custom image is auto-built.");
    return 1;
}

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
    var sessionSettings = new SessionSettings("WSLCCustomContainer", sessionPath)
    {
        CpuCount = 2,
        MemorySizeInMB = 2048,
        VhdRequirements = new VhdOptions(string.Empty, 4UL * 1024 * 1024 * 1024, VhdType.Dynamic),
    };

    using var session = new Session(sessionSettings);
    session.Start();

    // ---- Load the locally built image from the tar (no registry pull) ----
    Console.Error.WriteLine($"[wslc] Loading image from {Path.GetFileName(imageTarPath)}...");
    session.LoadImage(imageTarPath);

    // ---- Create & start container ----
    Console.Error.WriteLine("[wslc] Starting container...");

    // The init process keeps the container alive while we exec our tool.
    var initProcess = new ProcessSettings
    {
        CommandLine = new List<string> { "/bin/sleep", "infinity" },
    };

    var containerSettings = new ContainerSettings(imageName)
    {
        InitProcess = initProcess,
        EnableAutoRemove = true,
    };

    using var container = session.CreateContainer(containerSettings);
    container.Start();

    // ---- Exec the QR tool, passing the text to encode ----
    Console.Error.WriteLine($"[wslc] Generating QR code for: {text}");
    Console.Error.WriteLine();

    var processSettings = new ProcessSettings
    {
        CommandLine = new List<string> { "python", "/app/qr.py", text },
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
    stopEvent.Wait();

    Console.Error.WriteLine("[wslc] Shutting down...");
    container.Stop(Signal.SIGTERM, TimeSpan.FromSeconds(10));
    session.Terminate();
}
catch (Exception ex)
{
    Console.Error.WriteLine($"[wslc] Error: {ex.Message}");
}

return exitCode;
