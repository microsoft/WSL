// Copyright (C) Microsoft Corporation. All rights reserved.

// A minimal stand-in for a third-party application built on the WSL container SDK. It creates a
// session, runs a container whose workload crashes, and then carries on running.
//
// It exists to exercise Watson reporting for Linux crashes end to end. The application does
// nothing to request, collect or submit a report: the crash happens inside the utility VM and the
// resulting WSLAppCrash report is filed against this executable's file name simply because it is
// the process that asked for the container.

#include "wslcsdk.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

constexpr auto c_defaultImage = "debian:latest";
constexpr auto c_sessionName = L"wslc-crash-demo";
constexpr auto c_containerName = "wslc-crash-demo";

// Where the directory holding the workload binary is mounted inside the container.
constexpr auto c_payloadMountPath = "/payload";

// Copies the workload out of the read-only mount and runs it from the container's own filesystem,
// so the core the kernel writes belongs to a file that lives in the container rather than to one
// reached through a host mount.
//
// It runs as a background child rather than as the container's init process: init is pid 1 in the
// container's pid namespace, and the kernel discards signals whose disposition is the default
// action for it, so a fault there would be silently ignored.
std::string BuildWorkload(int Records)
{
    return "set -e\n"
           "cp /payload/crash /tmp/crash\n"
           "chmod +x /tmp/crash\n"
           "set +e\n"
           "/tmp/crash " +
           std::to_string(Records) +
           " &\n"
           "child=$!\n"
           "wait $child\n"
           "code=$?\n"
           "echo \"worker exited with $code\"\n"
           "exit $code\n";
}

double g_qpcFrequency = 1.0;

int64_t Now()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);

    return value.QuadPart;
}

double MillisecondsSince(int64_t Start)
{
    return static_cast<double>(Now() - Start) * 1000.0 / g_qpcFrequency;
}

void Check(const char* What, HRESULT Result, PWSTR ErrorMessage = nullptr)
{
    if (FAILED(Result))
    {
        wprintf(L"%hs failed: 0x%08x", What, Result);
        if (ErrorMessage != nullptr)
        {
            wprintf(L" (%s)", ErrorMessage);
        }

        wprintf(L"\n");
        exit(1);
    }
}

std::string Narrow(const std::wstring& Value)
{
    const auto size = WideCharToMultiByte(CP_UTF8, 0, Value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? size - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, Value.c_str(), -1, result.data(), size, nullptr, nullptr);

    return result;
}

std::wstring DefaultStoragePath()
{
    wchar_t buffer[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, ARRAYSIZE(buffer)) == 0)
    {
        return L".\\wslc-crash-demo";
    }

    return std::wstring{buffer} + L"\\wslc-crash-demo";
}

std::wstring ThisExecutableName()
{
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));

    return std::filesystem::path{buffer}.filename().wstring();
}

std::wstring PayloadDirectory()
{
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));

    return (std::filesystem::path{buffer}.parent_path() / L"payload").wstring();
}

void PrintUsage()
{
    wprintf(
        L"Creates a WSL container whose workload crashes.\n"
        L"\n"
        L"  --image <name>     Container image to run (default %hs).\n"
        L"  --storage <path>   Session storage directory (default %%LOCALAPPDATA%%\\wslc-crash-demo).\n"
        L"  --records <n>      Records for the workload to process (default 3). It faults on record\n"
        L"                     zero, so 0 runs it to completion without crashing.\n"
        L"  --help             Show this message.\n",
        c_defaultImage);
}

} // namespace

int wmain(int Argc, wchar_t** Argv)
{
    // The SDK does not initialize COM on the caller's behalf; without this WslcCreateSession
    // fails with CO_E_NOTINITIALIZED. The multi-threaded apartment is used because this process
    // blocks on waits rather than pumping messages, and crash dump callbacks arrive on a
    // background thread.
    const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Check("CoInitializeEx", comResult);

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_qpcFrequency = static_cast<double>(frequency.QuadPart);

    std::string image = c_defaultImage;
    std::wstring storagePath = DefaultStoragePath();
    int records = 3;

    for (int index = 1; index < Argc; index++)
    {
        const std::wstring argument = Argv[index];
        auto value = [&]() -> std::wstring {
            if (index + 1 >= Argc)
            {
                wprintf(L"Missing value for %s\n", argument.c_str());
                exit(1);
            }

            return Argv[++index];
        };

        if (argument == L"--image")
        {
            image = Narrow(value());
        }
        else if (argument == L"--storage")
        {
            storagePath = value();
        }
        else if (argument == L"--records")
        {
            records = _wtoi(value().c_str());
        }
        else if (argument == L"--help")
        {
            PrintUsage();
            return 0;
        }
        else
        {
            wprintf(L"Unrecognized argument: %s\n", argument.c_str());
            PrintUsage();
            return 1;
        }
    }

    const auto start = Now();

    const auto payloadDirectory = PayloadDirectory();
    const auto workloadBinary = std::filesystem::path{payloadDirectory} / L"crash";
    if (!std::filesystem::exists(workloadBinary))
    {
        wprintf(L"Workload binary not found at %s\n", workloadBinary.c_str());
        wprintf(L"It is cross compiled by the build; rebuild this project to produce it.\n");
        return 1;
    }

    wprintf(L"Application : %s (pid %u)\n", ThisExecutableName().c_str(), GetCurrentProcessId());
    wprintf(L"Image       : %hs\n", image.c_str());
    wprintf(L"Storage     : %s\n", storagePath.c_str());
    wprintf(L"Workload    : %s\n\n", workloadBinary.c_str());

    std::error_code error;
    std::filesystem::create_directories(storagePath, error);

    WslcSessionSettings sessionSettings{};
    Check("WslcInitSessionSettings", WslcInitSessionSettings(c_sessionName, storagePath.c_str(), &sessionSettings));

    PWSTR errorMessage = nullptr;
    WslcSession session = nullptr;
    wprintf(L"[t+%9.2f ms] creating session\n", MillisecondsSince(start));
    Check("WslcCreateSession", WslcCreateSession(&sessionSettings, &session, &errorMessage), errorMessage);

    WslcProcessSettings processSettings{};
    Check("WslcInitProcessSettings", WslcInitProcessSettings(&processSettings));

    const auto workload = BuildWorkload(records);

    const char* argv[] = {"/bin/sh", "-c", workload.c_str()};
    Check("WslcSetProcessSettingsCmdLine", WslcSetProcessSettingsCmdLine(&processSettings, argv, ARRAYSIZE(argv)));

    WslcContainerSettings containerSettings{};
    Check("WslcInitContainerSettings", WslcInitContainerSettings(image.c_str(), &containerSettings));
    Check("WslcSetContainerSettingsName", WslcSetContainerSettingsName(&containerSettings, c_containerName));
    Check("WslcSetContainerSettingsInitProcess", WslcSetContainerSettingsInitProcess(&containerSettings, &processSettings));

    // Read-only: the container copies the workload out before running it, so it never needs to
    // write back through the mount.
    const WslcContainerVolume volumes[] = {{payloadDirectory.c_str(), c_payloadMountPath, TRUE}};
    Check("WslcSetContainerSettingsVolumes", WslcSetContainerSettingsVolumes(&containerSettings, volumes, ARRAYSIZE(volumes)));

    wprintf(L"[t+%9.2f ms] creating container\n", MillisecondsSince(start));
    WslcContainer container = nullptr;
    auto result = WslcCreateContainer(session, &containerSettings, &container, &errorMessage);
    if (result == WSLC_E_IMAGE_NOT_FOUND)
    {
        wprintf(L"[t+%9.2f ms] image not present, pulling %hs\n", MillisecondsSince(start), image.c_str());

        WslcPullImageOptions pullOptions{};
        pullOptions.uri = image.c_str();
        Check("WslcPullSessionImage", WslcPullSessionImage(session, &pullOptions, &errorMessage), errorMessage);

        wprintf(L"[t+%9.2f ms] pull complete\n", MillisecondsSince(start));
        result = WslcCreateContainer(session, &containerSettings, &container, &errorMessage);
    }

    Check("WslcCreateContainer", result, errorMessage);

    char containerId[WSLC_CONTAINER_ID_BUFFER_SIZE]{};
    Check("WslcGetContainerID", WslcGetContainerID(container, containerId));
    wprintf(L"[t+%9.2f ms] container %.12hs created\n", MillisecondsSince(start), containerId);

    Check("WslcStartContainer", WslcStartContainer(container, WSLC_CONTAINER_START_FLAG_NONE, &errorMessage), errorMessage);
    wprintf(L"[t+%9.2f ms] container started\n", MillisecondsSince(start));

    WslcProcess initProcess = nullptr;
    Check("WslcGetContainerInitProcess", WslcGetContainerInitProcess(container, &initProcess));

    // The exit event belongs to the process object and must not be closed here.
    HANDLE exitEvent = nullptr;
    Check("WslcGetProcessExitEvent", WslcGetProcessExitEvent(initProcess, &exitEvent));

    if (WaitForSingleObject(exitEvent, 120 * 1000) != WAIT_OBJECT_0)
    {
        wprintf(L"Timed out waiting for the container to exit\n");
        return 1;
    }

    int32_t exitCode = 0;
    Check("WslcGetProcessExitCode", WslcGetProcessExitCode(initProcess, &exitCode));
    wprintf(L"[t+%9.2f ms] container exited with %d\n", MillisecondsSince(start), exitCode);

    WslcReleaseProcess(initProcess);
    WslcStopContainer(container, WSLC_SIGNAL_SIGKILL, 0, nullptr);
    WslcDeleteContainer(container, WSLC_DELETE_CONTAINER_FLAG_NONE, nullptr);
    WslcReleaseContainer(container);
    WslcTerminateSession(session);
    WslcReleaseSession(session);

    wprintf(L"[t+%9.2f ms] done\n", MillisecondsSince(start));

    return 0;
}
