// WSLC-HelloWorld
//
// The simplest possible WSL Container SDK sample, written in C using the flat
// C API (wslcsdk.h).  It starts a lightweight WSL container from a small Linux
// image and runs `echo` inside it, streaming the output back to the Windows
// console.

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <objbase.h>
#include "wslcsdk.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "wslcsdk.lib")

static const char* IMAGE_NAME = "alpine:latest";

// Process exit is signalled here so wmain can wait for it.
static HANDLE g_exitEvent = NULL;
static INT32 g_exitCode = -1;

static void PrintError(const wchar_t* context, HRESULT hr, PWSTR error)
{
    fwprintf(stderr, L"[wslc] Error: %s (0x%08X)", context, hr);
    if (error != NULL)
    {
        fwprintf(stderr, L": %s", error);
        CoTaskMemFree(error);
    }
    fwprintf(stderr, L"\n");
}

// Forward container stdout/stderr straight to the Windows console.
static void CALLBACK OnStdIO(WslcProcessIOHandle ioHandle, const BYTE* data, uint32_t dataSize, PVOID context)
{
    HANDLE output = (ioHandle == WSLC_PROCESS_IO_HANDLE_STDOUT) ? GetStdHandle(STD_OUTPUT_HANDLE) : GetStdHandle(STD_ERROR_HANDLE);
    DWORD written = 0;
    (void)context;
    WriteFile(output, data, dataSize, &written, NULL);
}

// Record the exit code and wake up wmain.
static void CALLBACK OnProcessExit(INT32 exitCode, PVOID context)
{
    (void)context;
    g_exitCode = exitCode;
    SetEvent(g_exitEvent);
}

// Build a storage path in a "WslcStorage" folder next to the executable, so the
// sample doesn't depend on any hard-coded absolute path.
static void GetStoragePath(wchar_t* buffer, size_t count)
{
    wchar_t exePath[MAX_PATH];
    wchar_t* lastSlash;
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash != NULL)
    {
        *(lastSlash + 1) = L'\0';
    }
    swprintf(buffer, count, L"%sWslcStorage", exePath);
}

int wmain(void)
{
    HRESULT hr;
    PWSTR error = NULL;
    int result = 1;

    WslcSession session = NULL;
    WslcContainer container = NULL;
    WslcProcess process = NULL;

    WslcSessionSettings sessionSettings;
    WslcContainerSettings containerSettings;
    WslcProcessSettings initProcess;
    WslcProcessSettings execProcess;
    WslcProcessCallbacks callbacks;
    WslcPullImageOptions pullOptions;
    wchar_t storagePath[MAX_PATH];

    PCSTR initArgv[2] = { "/bin/sleep", "60" };
    PCSTR echoArgv[2] = { "/bin/echo", "Hello, World from a WSL container!" };

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    g_exitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    // ---- Session ----
    fwprintf(stderr, L"[wslc] Creating session...\n");
    GetStoragePath(storagePath, ARRAYSIZE(storagePath));
    hr = WslcInitSessionSettings(L"WSLCHelloWorld", storagePath, &sessionSettings);
    if (FAILED(hr)) { PrintError(L"Init session settings", hr, NULL); goto cleanup; }

    hr = WslcCreateSession(&sessionSettings, &session, &error);
    if (FAILED(hr)) { PrintError(L"Create session", hr, error); goto cleanup; }

    // ---- Pull image ----
    fwprintf(stderr, L"[wslc] Pulling image '%hs'...\n", IMAGE_NAME);
    ZeroMemory(&pullOptions, sizeof(pullOptions));
    pullOptions.uri = IMAGE_NAME;
    hr = WslcPullSessionImage(session, &pullOptions, &error);
    if (FAILED(hr)) { PrintError(L"Pull image", hr, error); goto cleanup; }

    // ---- Create & start container ----
    fwprintf(stderr, L"[wslc] Starting container...\n");
    WslcInitProcessSettings(&initProcess);
    WslcSetProcessSettingsCmdLine(&initProcess, initArgv, 2);

    WslcInitContainerSettings(IMAGE_NAME, &containerSettings);
    WslcSetContainerSettingsName(&containerSettings, "wslc-helloworld");
    WslcSetContainerSettingsInitProcess(&containerSettings, &initProcess);
    WslcSetContainerSettingsFlags(&containerSettings, WSLC_CONTAINER_FLAG_AUTO_REMOVE);

    hr = WslcCreateContainer(session, &containerSettings, &container, &error);
    if (FAILED(hr)) { PrintError(L"Create container", hr, error); goto cleanup; }

    hr = WslcStartContainer(container, WSLC_CONTAINER_START_FLAG_NONE, &error);
    if (FAILED(hr)) { PrintError(L"Start container", hr, error); goto cleanup; }

    // ---- Run echo ----
    fwprintf(stderr, L"[wslc] Running echo...\n");
    WslcInitProcessSettings(&execProcess);
    WslcSetProcessSettingsCmdLine(&execProcess, echoArgv, 2);

    ZeroMemory(&callbacks, sizeof(callbacks));
    callbacks.onStdOut = OnStdIO;
    callbacks.onStdErr = OnStdIO;
    callbacks.onExit = OnProcessExit;
    WslcSetProcessSettingsCallbacks(&execProcess, &callbacks, NULL);

    hr = WslcCreateContainerProcess(container, &execProcess, &process, &error);
    if (FAILED(hr)) { PrintError(L"Run echo", hr, error); goto cleanup; }

    WaitForSingleObject(g_exitEvent, 30000);
    result = g_exitCode;

cleanup:
    fwprintf(stderr, L"[wslc] Shutting down...\n");

    if (process != NULL) { WslcReleaseProcess(process); }
    if (g_exitEvent != NULL) { CloseHandle(g_exitEvent); }
    if (container != NULL)
    {
        WslcStopContainer(container, WSLC_SIGNAL_SIGTERM, 5, NULL);
        WslcReleaseContainer(container);
    }
    if (session != NULL)
    {
        WslcTerminateSession(session);
        WslcReleaseSession(session);
    }

    fwprintf(stderr, L"[wslc] Done.\n");
    CoUninitialize();
    return result;
}
