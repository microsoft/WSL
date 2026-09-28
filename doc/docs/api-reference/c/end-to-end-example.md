# End-to-End Example

The example below shows one full lifecycle:

1. Initialize session settings
2. Create a session
3. Pull an image
4. Configure a container
5. Create and start the container
6. Inspect it
7. Create a second process
8. Stop and delete the container
9. Release handles and terminate the session

```c

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <objbase.h>
#include <filesystem>
#include "wslcsdk.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "wslcsdk.lib")

int main()
{
    // Initialize COM
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HRESULT hr;
    PWSTR error = nullptr;

    // 0. Check prerequisites
    WslcComponentFlags missing = WSLC_COMPONENT_FLAG_NONE;
    hr = WslcGetMissingComponents(&missing);
    if (FAILED(hr) || missing != WSLC_COMPONENT_FLAG_NONE) {
        printf("WSL components are missing. Run: wsl --install\n");
        CoUninitialize();
        return 1;
    }

    WslcVersion ver = {};
    WslcGetVersion(&ver);
    printf("WSL version: %u.%u.%u\n", ver.major, ver.minor, ver.revision);

    // 1. Initialize and create a session
    std::filesystem::path storagePath = std::filesystem::current_path();

    WslcSessionSettings sessionSettings;
    hr = WslcInitSessionSettings(L"MyApp", storagePath.c_str(), &sessionSettings);
    if (FAILED(hr)) return 1;

    // Optionally customize resources
    WslcSetSessionSettingsCpuCount(&sessionSettings, 4);
    WslcSetSessionSettingsMemory(&sessionSettings, 4096);

    WslcSession session = nullptr;
    hr = WslcCreateSession(&sessionSettings, &session, &error);
    if (FAILED(hr)) {
        wprintf(L"Session creation failed: %s\n", error ? error : L"unknown");
        CoTaskMemFree(error);
        CoUninitialize();
        return 1;
    }

    // 2. Pull an image
    WslcPullImageOptions pullOpts = {};
    pullOpts.uri = "docker.io/library/alpine:latest";
    hr = WslcPullSessionImage(session, &pullOpts, &error);
    if (FAILED(hr)) {
        wprintf(L"Pull failed: %s\n", error ? error : L"unknown");
        CoTaskMemFree(error);
        WslcTerminateSession(session);
        WslcReleaseSession(session);
        CoUninitialize();
        return 1;
    }

    // 3. Configure an init process
    WslcProcessSettings initProcSettings;
    WslcInitProcessSettings(&initProcSettings);

    PCSTR argv[] = { "/bin/echo", "Hello from WSL Container!" };
    WslcSetProcessSettingsCmdLine(&initProcSettings, argv, 2);

    // 4. Configure and create a container
    WslcContainerSettings containerSettings;
    WslcInitContainerSettings("alpine:latest", &containerSettings);
    WslcSetContainerSettingsName(&containerSettings, "hello-container");
    WslcSetContainerSettingsInitProcess(&containerSettings, &initProcSettings);

    WslcContainer container = nullptr;
    hr = WslcCreateContainer(session, &containerSettings, &container, &error);
    if (FAILED(hr)) {
        wprintf(L"Container creation failed: %s\n", error ? error : L"unknown");
        CoTaskMemFree(error);
        WslcTerminateSession(session);
        WslcReleaseSession(session);
        CoUninitialize();
        return 1;
    }

    // 5. Start the container
    hr = WslcStartContainer(container, WSLC_CONTAINER_START_FLAG_NONE, &error);
    if (FAILED(hr)) {
        wprintf(L"Start failed: %s\n", error ? error : L"unknown");
        CoTaskMemFree(error);
        WslcDeleteContainer(container, WSLC_DELETE_CONTAINER_FLAG_FORCE, nullptr);
        WslcReleaseContainer(container);
        WslcTerminateSession(session);
        WslcReleaseSession(session);
        CoUninitialize();
        return 1;
    }

    // 6. Wait for the init process to exit
    WslcProcess initProc = nullptr;
    hr = WslcGetContainerInitProcess(container, &initProc);
    if (SUCCEEDED(hr)) {
        HANDLE exitEvent = nullptr;
        if (SUCCEEDED(WslcGetProcessExitEvent(initProc, &exitEvent))) {
            WaitForSingleObject(exitEvent, 30000); // 30-second timeout
        }

        INT32 exitCode = 0;
        if (SUCCEEDED(WslcGetProcessExitCode(initProc, &exitCode))) {
            printf("Process exited with code: %d\n", exitCode);
        }
        WslcReleaseProcess(initProc);
    }

    // 7. Clean up
    WslcContainerState containerState = WSLC_CONTAINER_STATE_INVALID;
    if (SUCCEEDED(WslcGetContainerState(container, &containerState)) &&
        containerState == WSLC_CONTAINER_STATE_RUNNING) {
        WslcStopContainer(container, WSLC_SIGNAL_SIGTERM, 10, nullptr);
    }
    WslcDeleteContainer(container, WSLC_DELETE_CONTAINER_FLAG_NONE, nullptr);
    WslcReleaseContainer(container);
    WslcTerminateSession(session);
    WslcReleaseSession(session);

    CoUninitialize();
    return 0;
}
```
