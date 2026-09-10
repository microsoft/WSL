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

```cpp
#include <cstdio>
#include <string>
#include <chrono>
#include <winrt/Microsoft.WSL.Containers.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

using namespace winrt;
using namespace winrt::Microsoft::WSL::Containers;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;
using namespace std::chrono_literals;

int main()
{
    init_apartment();

    // 0. Check prerequisites
    auto missing = WslcService::GetMissingComponents();
    if (missing != static_cast<Component>(0))
    {
        printf("WSL components are missing. Run: wsl --install\n");
        return 1;
    }

    auto ver = WslcService::GetVersion();
    printf("WSL version: %u.%u.%u\n", ver.Major(), ver.Minor(), ver.Revision());

    // 1. Create a session
    SessionSettings sessionSettings{ L"MyApp", L"C:\\WslcData" };
    sessionSettings.CpuCount(4);
    sessionSettings.MemorySizeInMB(4096);

    Session session{ sessionSettings };
    session.Start();

    // 2. Pull an image
    PullImageOptions pullOpts{ L"docker.io/library/alpine:latest" };
    auto pullOp = session.PullImageAsync(pullOpts);
    co_await pullOp;

    // 3. Configure an init process
    ProcessSettings initProcSettings;
    initProcSettings.OutputMode(ProcessOutputMode::Event);
    auto argv = single_threaded_vector<hstring>();
    argv.Append(L"/bin/echo");
    argv.Append(L"Hello from WSL Container!");
    initProcSettings.CommandLine(argv);

    // 4. Configure and create a container
    ContainerSettings containerSettings{ L"alpine:latest" };
    containerSettings.Name(L"hello-container");
    containerSettings.InitProcess(initProcSettings);

    auto container = session.CreateContainer(containerSettings);

    // 5. Subscribe to init process events before starting
    auto initProcess = container.InitProcess();
    auto exitedEvent = handle{ CreateEvent(nullptr, TRUE, FALSE, nullptr) };
    int32_t initExitCode = -1;

    initProcess.OutputReceived([](auto const& data)
    {
        std::string text(data.begin(), data.end());
        printf("%s", text.c_str());
    });
    initProcess.Exited([&](int32_t exitCode)
    {
        initExitCode = exitCode;
        SetEvent(exitedEvent.get());
    });

    // 6. Start the container
    container.Start();

    // 7. Wait for the init process to exit (30-second timeout)
    WaitForSingleObject(exitedEvent.get(), 30000);
    printf("Process exited with code: %d\n", initExitCode);

    // 8. Clean up
    if (container.State() == ContainerState::Running)
    {
        container.Stop(Signal::SIGTERM, 10s);
    }
    container.Delete(DeleteContainerOption::None);
    session.Terminate();

    return 0;
}
```
