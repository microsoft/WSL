// Container.cpp - WSL Container API integration
// Manages the lifecycle of a Linux container using the WSL Container SDK.

#include "pch.h"
#include "wslcsdk.h"
#include "Container.h"

#include <objbase.h>
#include <filesystem>
#include <winrt/Windows.ApplicationModel.h>
#pragma comment(lib, "ws2_32.lib")

namespace
{
    std::wstring FormatHResult(HRESULT hr, const wchar_t* context)
    {
        wchar_t sysMsg[512] = {};
        FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, hr, 0, sysMsg, ARRAYSIZE(sysMsg), nullptr);
        wchar_t buf[1024];
        swprintf_s(buf, L"%s (0x%08X: %s)", context, static_cast<unsigned int>(hr), sysMsg);
        return buf;
    }
}

namespace winrt::WSLAMoviePlayer::implementation
{
    Container::Container()
        : m_isRunning(false)
        , m_session(nullptr)
        , m_container(nullptr)
        , m_process(nullptr)
    {
    }

    Container::~Container()
    {
        Stop();
    }

    void Container::Log(const std::wstring& msg)
    {
        OutputDebugStringW((msg + L"\n").c_str());
        if (OnContainerLog) OnContainerLog(winrt::hstring(msg));
    }

    winrt::Windows::Foundation::IAsyncAction Container::StartAsync()
    {
        if (m_isRunning)
        {
            Log(L"[Container] Already running");
            co_return;
        }

        co_await winrt::resume_background();

        HRESULT hr = S_OK;
        WslcSessionSettings sessionSettings = {};
        WslcContainerSettings containerSettings = {};
        PWSTR errorMessage = nullptr;
        PWSTR containerErrorMessage = nullptr;

        hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        {
            Log(L"[Container] COM initialization failed");
            if (OnContainerError) OnContainerError(L"COM initialization failed");
            co_return;
        }

        try
        {
            auto localFolder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
            std::wstring storagePath = std::wstring(localFolder.Path().c_str()) + L"\\wslc-storage";
            std::filesystem::create_directories(storagePath);
            Log(L"[Container] Storage path: " + storagePath);

            Log(L"[Container] Calling WslcInitSessionSettings...");
            hr = WslcInitSessionSettings(L"WSLAMoviePlayer_Session", storagePath.c_str(), &sessionSettings);
            if (FAILED(hr))
            {
                auto msg = FormatHResult(hr, L"WslcInitSessionSettings failed");
                Log(L"[Container] " + msg);
                if (OnContainerError) OnContainerError(winrt::hstring(msg));
                co_return;
            }
            Log(L"[Container] Session settings initialized OK");

            Log(L"[Container] Calling WslcSetSessionSettingsVHD (15 GB dynamic)...");
            WslcVhdRequirements vhdReqs = {};
            vhdReqs.sizeInBytes = 15ULL * 1024 * 1024 * 1024;
            vhdReqs.type = WSLC_VHD_TYPE_DYNAMIC;
            hr = WslcSetSessionSettingsVHD(&sessionSettings, &vhdReqs);
            if (FAILED(hr))
            {
                auto msg = FormatHResult(hr, L"WslcSetSessionSettingsVHD failed");
                Log(L"[Container] " + msg);
                if (OnContainerError) OnContainerError(winrt::hstring(msg));
                co_return;
            }
            Log(L"[Container] VHD settings OK");

            Log(L"[Container] Calling WslcSetSessionSettingsFeatureFlags (GPU)...");
            hr = WslcSetSessionSettingsFeatureFlags(&sessionSettings,
                WslcSessionFeatureFlags::WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU);
            if (FAILED(hr))
            {
                Log(L"[Container] GPU flag failed (non-fatal) - hr=0x" + std::to_wstring(hr));
            }
            else
            {
                Log(L"[Container] GPU flag set OK");
            }

            Log(L"[Container] Calling WslcCreateSession...");
            hr = WslcCreateSession(&sessionSettings, &m_session, &errorMessage);
            if (FAILED(hr))
            {
                std::wstring detail = L"WslcCreateSession failed";
                if (errorMessage)
                {
                    detail += L": ";
                    detail += errorMessage;
                    CoTaskMemFree(errorMessage);
                    errorMessage = nullptr;
                }
                auto msg = FormatHResult(hr, detail.c_str());
                Log(L"[Container] " + msg);
                if (OnContainerError) OnContainerError(winrt::hstring(msg));
                co_return;
            }
            Log(L"[Container] Session created OK");

            Log(L"[Container] Calling WslcContainerInitSettings(\"subtitler:latest\")...");
            hr = WslcContainerInitSettings("subtitler:latest", &containerSettings);
            if (FAILED(hr))
            {
                auto msg = FormatHResult(hr, L"WslcContainerInitSettings failed");
                Log(L"[Container] " + msg);
                if (OnContainerError) OnContainerError(winrt::hstring(msg));
                co_return;
            }
            Log(L"[Container] Container settings initialized OK");

            Log(L"[Container] Setting container name, GPU flag, port mapping...");
            WslcContainerSettingsSetName(&containerSettings, "WSLAMoviePlayer_container");
            WslcContainerSettingsSetFlags(&containerSettings,
                WslcContainerFlags::WSLC_CONTAINER_FLAG_ENABLE_GPU);

            WslcContainerPortMapping portMapping = {};
            portMapping.windowsPort = 8000;
            portMapping.containerPort = 8000;
            portMapping.protocol = WslcPortProtocol::WSLC_PORT_PROTOCOL_TCP;
            portMapping.windowsAddress = nullptr;
            WslcContainerSettingsSetPortMapping(&containerSettings, &portMapping, 1);

            Log(L"[Container] Setting init process...");
            WslcProcessSettings processSettings = {};
            hr = WslcProcessInitSettings(&processSettings);
            if (FAILED(hr))
            {
                auto msg = FormatHResult(hr, L"WslcProcessInitSettings failed");
                Log(L"[Container] " + msg);
                if (OnContainerError) OnContainerError(winrt::hstring(msg));
                co_return;
            }
            PCSTR args[] = { "/bin/sh", "-c", "echo Container started && sleep infinity" };
            hr = WslcProcessSettingsSetCmdLineArgs(&processSettings, args, _countof(args));
            Log(L"[Container] SetCmdLineArgs hr=0x" + std::to_wstring(static_cast<unsigned int>(hr)));
            hr = WslcContainerSettingsSetInitProcess(&containerSettings, &processSettings);
            Log(L"[Container] SetInitProcess hr=0x" + std::to_wstring(static_cast<unsigned int>(hr)));
            Log(L"[Container] Container options set OK");

            Log(L"[Container] Calling WslcContainerCreate...");
            hr = WslcContainerCreate(m_session, &containerSettings, &m_container, &containerErrorMessage);
            if (FAILED(hr))
            {
                std::wstring detail = L"WslcContainerCreate failed";
                if (containerErrorMessage)
                {
                    detail += L": ";
                    detail += containerErrorMessage;
                    CoTaskMemFree(containerErrorMessage);
                    containerErrorMessage = nullptr;
                }
                auto msg = FormatHResult(hr, detail.c_str());
                Log(L"[Container] " + msg);
                if (OnContainerError) OnContainerError(winrt::hstring(msg));
                co_return;
            }
            Log(L"[Container] Container created OK");

            Log(L"[Container] Calling WslcContainerStart...");
            hr = WslcContainerStart(m_container, WslcContainerStartFlags::WSLC_CONTAINER_START_FLAG_NONE);
            if (FAILED(hr))
            {
                auto msg = FormatHResult(hr, L"WslcContainerStart failed");
                Log(L"[Container] " + msg);
                if (OnContainerError) OnContainerError(winrt::hstring(msg));
                co_return;
            }

            m_isRunning = true;
            Log(L"[Container] Container started successfully (port 8000 mapped)");

            if (OnContainerStarted)
            {
                OnContainerStarted();
            }

            hr = WslcContainerGetInitProcess(m_container, &m_process);
            if (SUCCEEDED(hr))
            {
                Log(L"[Container] Got init process, waiting for exit...");
                HANDLE exitEvent = nullptr;
                hr = WslcProcessGetExitEvent(m_process, &exitEvent);
                if (SUCCEEDED(hr) && exitEvent)
                {
                    WaitForSingleObject(exitEvent, INFINITE);

                    INT32 exitCode = -1;
                    WslcProcessGetExitCode(m_process, &exitCode);
                    Log(L"[Container] Process exited with code " + std::to_wstring(exitCode));

                    if (OnContainerOutput)
                    {
                        wchar_t msg[64];
                        swprintf_s(msg, L"Exited (code %d)", exitCode);
                        OnContainerOutput(winrt::hstring(msg));
                    }
                }
            }
            else
            {
                auto msg = FormatHResult(hr, L"WslcContainerGetInitProcess failed");
                Log(L"[Container] " + msg);
            }

            Log(L"[Container] Container process finished");
        }
        catch (...)
        {
            Log(L"[Container] Unexpected exception during startup");
            if (OnContainerError) OnContainerError(L"Unexpected exception");
        }

        if (errorMessage) CoTaskMemFree(errorMessage);
        if (containerErrorMessage) CoTaskMemFree(containerErrorMessage);

        Cleanup();

        if (OnContainerStopped)
        {
            OnContainerStopped();
        }
    }

    void Container::Stop()
    {
        if (!m_isRunning && !m_session && !m_container)
        {
            return;
        }

        OutputDebugStringW(L"Container: Stopping...\n");
        Cleanup();

        if (OnContainerStopped)
        {
            OnContainerStopped();
        }
    }

    bool Container::IsRunning() const
    {
        return m_isRunning;
    }

    void Container::Cleanup()
    {
        if (m_process)
        {
            WslcProcessRelease(m_process);
            m_process = nullptr;
        }
        if (m_container)
        {
            OutputDebugStringW(L"Container: Stopping and deleting container...\n");
            WslcContainerStop(m_container, WslcSignal::WSLC_SIGNAL_SIGKILL, 0);
            WslcContainerDelete(m_container, WSLC_DELETE_CONTAINER_FLAG_NONE);
            WslcContainerRelease(m_container);
            m_container = nullptr;
        }
        if (m_session)
        {
            OutputDebugStringW(L"Container: Terminating session...\n");
            WslcTerminateSession(m_session);
            WslcReleaseSession(m_session);
            m_session = nullptr;
        }

        m_isRunning = false;
        OutputDebugStringW(L"Container: Cleanup complete\n");
    }
}
