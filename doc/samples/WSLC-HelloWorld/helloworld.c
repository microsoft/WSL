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
    FILE* output = (ioHandle == WSLC_PROCESS_IO_HANDLE_STDOUT) ? stdout : stderr;
    (void)context;
    fprintf(output, "%.*s", (int)dataSize, (const char*)data);
    fflush(output);
}

// Record the exit code and wake up wmain.
static void CALLBACK OnProcessExit(INT32 exitCode, PVOID context)
{
    (void)context;
    g_exitCode = exitCode;
    if (!SetEvent(g_exitEvent))
    {
        fwprintf(stderr, L"[wslc] Warning: SetEvent failed (0x%08X)\n", GetLastError());
    }
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
    DWORD waitResult;

    PCSTR initArgv[2] = {"/bin/sleep", "60"};
    PCSTR echoArgv[2] = {"/bin/echo", "Hello, World from a WSL container!"};

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        PrintError(L"Initialize COM", hr, NULL);
        return 1;
    }

    g_exitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_exitEvent == NULL)
    {
        PrintError(L"Create exit event", HRESULT_FROM_WIN32(GetLastError()), NULL);
        goto cleanup;
    }

    // ---- Session ----
    fwprintf(stderr, L"[wslc] Creating session...\n");
    GetStoragePath(storagePath, ARRAYSIZE(storagePath));
    hr = WslcInitSessionSettings(L"WSLCHelloWorld", storagePath, &sessionSettings);
    if (FAILED(hr))
    {
        PrintError(L"Init session settings", hr, NULL);
        goto cleanup;
    }

    hr = WslcCreateSession(&sessionSettings, &session, &error);
    if (FAILED(hr))
    {
        PrintError(L"Create session", hr, error);
        goto cleanup;
    }

    // ---- Pull image ----
    fwprintf(stderr, L"[wslc] Pulling image '%hs'...\n", IMAGE_NAME);
    ZeroMemory(&pullOptions, sizeof(pullOptions));
    pullOptions.uri = IMAGE_NAME;
    hr = WslcPullSessionImage(session, &pullOptions, &error);
    if (FAILED(hr))
    {
        PrintError(L"Pull image", hr, error);
        goto cleanup;
    }

    // ---- Create & start container ----
    fwprintf(stderr, L"[wslc] Starting container...\n");
    WslcInitProcessSettings(&initProcess);
    WslcSetProcessSettingsCmdLine(&initProcess, initArgv, 2);

    WslcInitContainerSettings(IMAGE_NAME, &containerSettings);
    WslcSetContainerSettingsName(&containerSettings, "wslc-helloworld");
    WslcSetContainerSettingsInitProcess(&containerSettings, &initProcess);
    WslcSetContainerSettingsFlags(&containerSettings, WSLC_CONTAINER_FLAG_AUTO_REMOVE);

    hr = WslcCreateContainer(session, &containerSettings, &container, &error);
    if (FAILED(hr))
    {
        PrintError(L"Create container", hr, error);
        goto cleanup;
    }

    hr = WslcStartContainer(container, WSLC_CONTAINER_START_FLAG_NONE, &error);
    if (FAILED(hr))
    {
        PrintError(L"Start container", hr, error);
        goto cleanup;
    }

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
    if (FAILED(hr))
    {
        PrintError(L"Run echo", hr, error);
        goto cleanup;
    }

    waitResult = WaitForSingleObject(g_exitEvent, 30000);
    if (waitResult == WAIT_OBJECT_0)
    {
        result = g_exitCode;
    }
    else if (waitResult == WAIT_TIMEOUT)
    {
        fwprintf(stderr, L"[wslc] Error: Timed out waiting for the process to exit.\n");
    }
    else
    {
        PrintError(L"Wait for process exit", HRESULT_FROM_WIN32(GetLastError()), NULL);
    }

cleanup:
    fwprintf(stderr, L"[wslc] Shutting down...\n");

    if (process != NULL)
    {
        WslcReleaseProcess(process);
    }
    if (g_exitEvent != NULL)
    {
        CloseHandle(g_exitEvent);
    }
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
