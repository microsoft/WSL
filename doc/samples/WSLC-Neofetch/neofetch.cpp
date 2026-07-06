// WSLC-Neofetch
//
// A Windows executable that wraps the Linux "neofetch" command using the
// WSL Container SDK, written with the modern C++/WinRT projection
// (winrt/Microsoft.WSL.Containers.h) rather than the flat C API.
//
// All command-line arguments are forwarded into the container, so
// "neofetch.exe --help" behaves just like "neofetch --help" on Linux.

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.WSL.Containers.h>

#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace winrt::Microsoft::WSL::Containers;

namespace {
constexpr std::wstring_view c_imageName = L"anrginit/ubuntu-neofetch:1.0";

// Forward a chunk of container stdout/stderr straight to the Windows console.
void WriteToConsole(FILE* stream, array_view<uint8_t const> data)
{
    fprintf(stream, "%.*s", static_cast<int>(data.size()), reinterpret_cast<const char*>(data.data()));
    fflush(stream);
}

// Build a storage path in a "WslcStorage" folder next to the executable, so
// the sample doesn't depend on any hard-coded absolute path.
std::wstring GetStoragePath()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash != nullptr)
    {
        *(lastSlash + 1) = L'\0';
    }
    return std::wstring{exePath} + L"WslcStorage";
}
} // namespace

int wmain(int argc, wchar_t* argv[])
{
    init_apartment();

    // argv[0] (our exe) is replaced with "neofetch"; the rest pass through.
    std::vector<hstring> commandLine{L"neofetch"};
    for (int i = 1; i < argc; ++i)
    {
        commandLine.emplace_back(argv[i]);
    }

    try
    {
        // ---- Session ----
        fwprintf(stderr, L"[wslc] Creating session...\n");
        SessionSettings sessionSettings{L"WSLCNeofetch", GetStoragePath()};
        sessionSettings.CpuCount(4);
        sessionSettings.MemorySizeInMB(2048);

        Session session{sessionSettings};
        session.Start();

        // ---- Pull image ----
        fwprintf(stderr, L"[wslc] Pulling image '%ls'...\n", c_imageName.data());
        session.PullImage(PullImageOptions{hstring{c_imageName}});

        // ---- Create & start container ----
        // The init process keeps the container alive while we exec neofetch.
        fwprintf(stderr, L"[wslc] Starting container...\n");
        ProcessSettings initProcess;
        initProcess.CommandLine(single_threaded_vector<hstring>({L"/bin/sleep", L"60"}));

        ContainerSettings containerSettings{hstring{c_imageName}};
        containerSettings.Name(L"wslc-neofetch");
        containerSettings.InitProcess(initProcess);
        containerSettings.EnableAutoRemove(true);

        Container container = session.CreateContainer(containerSettings);
        container.Start();

        // ---- Exec neofetch ----
        fwprintf(stderr, L"[wslc] Running neofetch...\n");
        ProcessSettings processSettings;
        processSettings.CommandLine(single_threaded_vector<hstring>(std::move(commandLine)));
        processSettings.OutputMode(ProcessOutputMode::Event);

        Process process = container.CreateProcess(processSettings);

        handle exitEvent{CreateEvent(nullptr, TRUE, FALSE, nullptr)};
        if (!exitEvent)
        {
            throw_last_error();
        }
        int32_t exitCode = -1;

        process.OutputReceived([](array_view<uint8_t const> data) { WriteToConsole(stdout, data); });
        process.ErrorReceived([](array_view<uint8_t const> data) { WriteToConsole(stderr, data); });
        process.Exited([&](int32_t code) {
            exitCode = code;
            if (!SetEvent(exitEvent.get()))
            {
                fwprintf(stderr, L"[wslc] Warning: SetEvent failed (0x%08X)\n", GetLastError());
            }
        });

        process.Start();
        DWORD waitResult = WaitForSingleObject(exitEvent.get(), 30000);
        if (waitResult == WAIT_TIMEOUT)
        {
            fwprintf(stderr, L"[wslc] Error: Timed out waiting for the process to exit.\n");
        }
        else if (waitResult != WAIT_OBJECT_0)
        {
            throw_last_error();
        }

        // ---- Cleanup ----
        fwprintf(stderr, L"[wslc] Shutting down...\n");
        container.Stop(Signal::SIGTERM, std::chrono::seconds{5});
        session.Terminate();

        fwprintf(stderr, L"[wslc] Done.\n");
        return exitCode;
    }
    catch (hresult_error const& ex)
    {
        fwprintf(stderr, L"[wslc] Error: %ls (0x%08X)\n", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
        return 1;
    }
}
