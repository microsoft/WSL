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

namespace
{
    constexpr std::wstring_view c_imageName = L"anrginit/ubuntu-neofetch:1.0";

    // Forward a chunk of container stdout/stderr straight to the Windows console.
    void WriteToConsole(DWORD stdHandle, array_view<uint8_t const> data)
    {
        DWORD written = 0;
        WriteFile(GetStdHandle(stdHandle), data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
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
        return std::wstring{ exePath } + L"WslcStorage";
    }
}

int wmain(int argc, wchar_t* argv[])
{
    init_apartment();

    // argv[0] (our exe) is replaced with "neofetch"; the rest pass through.
    std::vector<hstring> commandLine{ L"neofetch" };
    for (int i = 1; i < argc; ++i)
    {
        commandLine.emplace_back(argv[i]);
    }

    try
    {
        // ---- Session ----
        fwprintf(stderr, L"[wslc] Creating session...\n");
        SessionSettings sessionSettings{ L"WSLCNeofetch", GetStoragePath() };
        sessionSettings.CpuCount(4);
        sessionSettings.MemorySizeInMB(2048);

        Session session{ sessionSettings };
        session.Start();

        // ---- Pull image ----
        fwprintf(stderr, L"[wslc] Pulling image '%ls'...\n", c_imageName.data());
        session.PullImage(PullImageOptions{ hstring{ c_imageName } });

        // ---- Create & start container ----
        // The init process keeps the container alive while we exec neofetch.
        fwprintf(stderr, L"[wslc] Starting container...\n");
        ProcessSettings initProcess;
        initProcess.CommandLine(single_threaded_vector<hstring>({ L"/bin/sleep", L"60" }));

        ContainerSettings containerSettings{ hstring{ c_imageName } };
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

        handle exitEvent{ CreateEvent(nullptr, TRUE, FALSE, nullptr) };
        int32_t exitCode = -1;

        process.OutputReceived([](array_view<uint8_t const> data) { WriteToConsole(STD_OUTPUT_HANDLE, data); });
        process.ErrorReceived([](array_view<uint8_t const> data) { WriteToConsole(STD_ERROR_HANDLE, data); });
        process.Exited([&](int32_t code) {
            exitCode = code;
            SetEvent(exitEvent.get());
        });

        process.Start();
        WaitForSingleObject(exitEvent.get(), 30000);

        // ---- Cleanup ----
        fwprintf(stderr, L"[wslc] Shutting down...\n");
        container.Stop(Signal::SIGTERM, std::chrono::seconds{ 5 });
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
