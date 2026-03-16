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
        OutputDebugStringW(L"Container: Creating new Container instance\n");
    }

    Container::~Container()
    {
        OutputDebugStringW(L"Container: Destroying Container instance\n");
        Stop();
    }

    winrt::Windows::Foundation::IAsyncAction Container::StartAsync()
    {
        if (m_isRunning)
        {
            OutputDebugStringW(L"Container: Already running\n");
            co_return;
        }

        // Switch to background thread for blocking WSL Container API calls
        co_await winrt::resume_background();

        HRESULT hr = S_OK;
        WslcSessionSettings sessionSettings = {};
        WslcContainerSettings containerSettings = {};
        PWSTR errorMessage = nullptr;
        PWSTR containerErrorMessage = nullptr;
        HANDLE fileHandle = nullptr;

        // Ensure COM is initialized on this thread
        hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        {
            OutputDebugStringW(L"Container: COM initialization failed\n");
            if (OnContainerError)
            {
                OnContainerError(L"COM initialization failed");
            }
            co_return;
        }

        try
        {
            // Step 1: Initialize session settings
            OutputDebugStringW(L"Container: Initializing session settings...\n");

            // Use the app's local data folder (packaged apps can't write to current_path which is System32)
            auto localFolder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
            std::wstring storagePath = std::wstring(localFolder.Path().c_str()) + L"\\wslc-storage";
            std::filesystem::create_directories(storagePath);
            OutputDebugStringW((L"Container: Storage path: " + storagePath + L"\n").c_str());

            hr = WslcInitSessionSettings(L"WSLAMoviePlayer_Session", storagePath.c_str(), &sessionSettings);
            if (FAILED(hr))
            {
                auto msg = FormatHResult(hr, L"Failed to initialize session settings");
                OutputDebugStringW((L"Container: " + msg + L"\n").c_str());
                if (OnContainerError) OnContainerError(winrt::hstring(msg));
                co_return;
            }

            // Step 2: Enable GPU support
            OutputDebugStringW(L"Container: Enabling GPU support...\n");
            hr = WslcSetSessionSettingsFeatureFlags(&sessionSettings,
                WslcSessionFeatureFlags::WSLC_SESSION_FEATURE_FLAG_ENABLE_GPU);
            if (FAILED(hr))
            {
                OutputDebugStringW(L"Container: Failed to set GPU feature flag (non-fatal)\n");
            }

            // Step 3: Create session
            OutputDebugStringW(L"Container: Creating session...\n");
            hr = WslcCreateSession(&sessionSettings, &m_session, &errorMessage);
            if (FAILED(hr))
            {
                std::wstring detail = L"Failed to create session";
                if (errorMessage)
                {
                    detail += L": ";
                    detail += errorMessage;
                    CoTaskMemFree(errorMessage);
                    errorMessage = nullptr;
                }
                auto msg = FormatHResult(hr, detail.c_str());
                OutputDebugStringW((L"Container: " + msg + L"\n").c_str());
                if (OnContainerError) OnContainerError(winrt::hstring(msg));
                co_return;
            }
            OutputDebugStringW(L"Container: Session created\n");

            // Step 4: Import container image from local tar file
            OutputDebugStringW(L"Container: Importing image from subtitler.tar...\n");

            // Get the package install directory where subtitler.tar is deployed
            auto packageFolder = winrt::Windows::ApplicationModel::Package::Current().InstalledLocation();
            std::wstring tarPath = std::wstring(packageFolder.Path().c_str()) + L"\\subtitler.tar";
            OutputDebugStringW((L"Container: Image path: " + tarPath + L"\n").c_str());

            WslcImportImageOptions importOptions = {};
            importOptions.imagePath = tarPath.c_str();
            hr = WslcSessionImageImport(m_session, &importOptions);
            if (FAILED(hr))
            {
                auto msg = FormatHResult(hr, L"Failed to import image");
                OutputDebugStringW((L"Container: " + msg + L"\n").c_str());
                if (OnContainerError) OnContainerError(winrt::hstring(msg));
                co_return;
            }
            OutputDebugStringW(L"Container: Image imported successfully\n");

            // Step 5: Configure container
            OutputDebugStringW(L"Container: Configuring container...\n");
            hr = WslcContainerInitSettings("subtitler:latest", &containerSettings);
            if (FAILED(hr))
            {
                auto msg = FormatHResult(hr, L"Failed to init container settings");
                OutputDebugStringW((L"Container: " + msg + L"\n").c_str());
                if (OnContainerError) OnContainerError(winrt::hstring(msg));
                co_return;
            }

            WslcContainerSettingsSetName(&containerSettings, "WSLAMoviePlayer_container");
            WslcContainerSettingsSetFlags(&containerSettings,
                WslcContainerFlags::WSLC_CONTAINER_FLAG_ENABLE_GPU);

            // Step 6: Enable bridged networking and map port 8000
            OutputDebugStringW(L"Container: Enabling bridged networking with port 8000...\n");
            WslcContainerSettingsSetNetworkingMode(&containerSettings,
                WslcContainerNetworkingMode::WSLC_CONTAINER_NETWORKING_MODE_BRIDGED);

            WslcContainerPortMapping portMapping = {};
            portMapping.windowsPort = 8000;
            portMapping.containerPort = 8000;
            portMapping.protocol = WslcPortProtocol::WSLC_PORT_PROTOCOL_TCP;
            portMapping.windowsAddress = nullptr;
            WslcContainerSettingsSetPortMapping(&containerSettings, &portMapping, 1);

            // Step 7: Create container
            OutputDebugStringW(L"Container: Creating container...\n");
            hr = WslcContainerCreate(m_session, &containerSettings, &m_container, &containerErrorMessage);
            if (FAILED(hr))
            {
                std::wstring detail = L"Failed to create container";
                if (containerErrorMessage)
                {
                    detail += L": ";
                    detail += containerErrorMessage;
                    CoTaskMemFree(containerErrorMessage);
                    containerErrorMessage = nullptr;
                }
                auto msg = FormatHResult(hr, detail.c_str());
                OutputDebugStringW((L"Container: " + msg + L"\n").c_str());
                if (OnContainerError) OnContainerError(winrt::hstring(msg));
                co_return;
            }
            OutputDebugStringW(L"Container: Container created\n");

            // Step 8: Start container (no ATTACH flag — this is a long-running service)
            OutputDebugStringW(L"Container: Starting container...\n");
            hr = WslcContainerStart(m_container, WslcContainerStartFlags::WSLC_CONTAINER_START_FLAG_NONE);
            if (FAILED(hr))
            {
                auto msg = FormatHResult(hr, L"Failed to start container");
                OutputDebugStringW((L"Container: " + msg + L"\n").c_str());
                if (OnContainerError) OnContainerError(winrt::hstring(msg));
                co_return;
            }

            m_isRunning = true;
            OutputDebugStringW(L"Container: Container started successfully (port 8000 mapped)\n");

            if (OnContainerStarted)
            {
                OnContainerStarted();
            }

            // Step 9: Wait for the container process to exit
            hr = WslcContainerGetInitProcess(m_container, &m_process);
            if (SUCCEEDED(hr))
            {
                HANDLE exitEvent = nullptr;
                hr = WslcProcessGetExitEvent(m_process, &exitEvent);
                if (SUCCEEDED(hr) && exitEvent)
                {
                    OutputDebugStringW(L"Container: Waiting for container process to exit...\n");
                    WaitForSingleObject(exitEvent, INFINITE);

                    INT32 exitCode = -1;
                    WslcProcessGetExitCode(m_process, &exitCode);
                    wchar_t exitBuf[128];
                    swprintf_s(exitBuf, L"Container: Process exited with code %d\n", exitCode);
                    OutputDebugStringW(exitBuf);

                    if (OnContainerOutput)
                    {
                        wchar_t msg[64];
                        swprintf_s(msg, L"Exited (code %d)", exitCode);
                        OnContainerOutput(winrt::hstring(msg));
                    }
                }
            }

            OutputDebugStringW(L"Container: Container process finished\n");
        }
        catch (...)
        {
            OutputDebugStringW(L"Container: Unexpected exception during startup\n");
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
