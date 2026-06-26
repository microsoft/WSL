# End-to-End Example

The example below shows one full lifecycle matching the C API example:

1. Check prerequisites
2. Print SDK version
3. Create a session (4 CPUs, 4 GB RAM)
4. Pull alpine:latest
5. Configure an init process (`/bin/echo "Hello from WSL Container!"`)
6. Create and start the container
7. Wait for the init process to exit
8. Print exit code
9. Stop and delete the container
10. Terminate the session

```csharp
using Microsoft.WSL.Containers;
using System;
using System.Text;
using System.Threading.Tasks;

class Program
{
    static async Task<int> Main()
    {
        // 0. Check prerequisites
        var missing = WslcService.GetMissingComponents();
        if (missing.Count > 0)
        {
            Console.WriteLine("WSL components are missing. Run: wsl --install");
            return 1;
        }

        var ver = WslcService.GetVersion();
        Console.WriteLine($"WSL version: {ver.Major}.{ver.Minor}.{ver.Revision}");

        // 1. Create a session
        var sessionSettings = new SessionSettings("MyApp", @"C:\WslcData")
        {
            CpuCount = 4,
            MemorySizeInMB = 4096
        };

        var session = new Session(sessionSettings);
        session.Start();

        // 2. Pull an image
        var pullOp = session.PullImageAsync(new PullImageOptions("docker.io/library/alpine:latest"));
        pullOp.Progress = (op, progress) =>
            Console.WriteLine($"Pull: {progress.Status} {progress.CurrentBytes}/{progress.TotalBytes}");
        await pullOp;

        // 3. Configure an init process
        var initProcSettings = new ProcessSettings
        {
            CommandLine = new[] { "/bin/echo", "Hello from WSL Container!" },
            OutputMode = ProcessOutputMode.Event
        };

        // 4. Configure and create a container
        var containerSettings = new ContainerSettings("alpine:latest")
        {
            Name = "hello-container",
            InitProcess = initProcSettings
        };

        var container = session.CreateContainer(containerSettings);

        // 5. Subscribe to init process events before starting
        var exited = new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);

        container.InitProcess.OutputReceived += data =>
            Console.Write(Encoding.UTF8.GetString(data));
        container.InitProcess.Exited += code =>
            exited.TrySetResult(code);

        // 6. Start the container
        container.Start();

        // 7. Wait for the init process to exit (30-second timeout)
        var completed = await Task.WhenAny(exited.Task, Task.Delay(TimeSpan.FromSeconds(30)));
        int exitCode = completed == exited.Task ? exited.Task.Result : -1;
        Console.WriteLine($"Process exited with code: {exitCode}");

        // 8. Clean up
        if (container.State == ContainerState.Running)
        {
            container.Stop(Signal.SIGTERM, TimeSpan.FromSeconds(10));
        }
        container.Delete(DeleteContainerOption.None);
        session.Terminate();

        return exitCode;
    }
}
```
