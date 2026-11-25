// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "wslc.h"
#include <winrt/Microsoft.WSL.Containers.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

namespace WSLCSDK = winrt::Microsoft::WSL::Containers;

namespace {

constexpr DWORD c_completionTimeoutMs = 2 * 60 * 1000;

template <typename T>
T GetFactory(HMODULE module, const wchar_t* className)
{
    using GetActivationFactory = HRESULT(WINAPI*)(void*, void**);
    auto getFactory = reinterpret_cast<GetActivationFactory>(GetProcAddress(module, "DllGetActivationFactory"));
    THROW_LAST_ERROR_IF_NULL(getFactory);

    winrt::Windows::Foundation::IActivationFactory factory{nullptr};
    winrt::check_hresult(getFactory(winrt::get_abi(winrt::hstring{className}), winrt::put_abi(factory)));
    return factory.as<T>();
}

void VerifyUnload(wil::unique_hmodule module, const std::filesystem::path& modulePath, WSLCSDK::Process process, winrt::Windows::Foundation::IUnknown owner)
{
    struct CallbackState
    {
        wil::unique_event releaseReturned{wil::EventOptions::ManualReset};
        wil::unique_event resumeCallback{wil::EventOptions::ManualReset};
        wil::unique_event callbackReturned{wil::EventOptions::ManualReset};
        std::atomic<bool> callbackTimedOut{false};
    };

    auto state = std::make_shared<CallbackState>();
    auto resumeOnExit = wil::scope_exit([&]() { state->resumeCallback.SetEvent(); });
    process.OutputReceived([owner = std::move(owner), state](winrt::array_view<const uint8_t>) mutable {
        if (!owner)
        {
            return;
        }

        owner = nullptr;
        state->releaseReturned.SetEvent();
        state->callbackTimedOut = !state->resumeCallback.wait(c_completionTimeoutMs);
        state->callbackReturned.SetEvent();
    });

    {
        auto input = process.GetInputStream();
        process = nullptr;
        winrt::Windows::Storage::Streams::DataWriter writer{input};
        writer.WriteString(L"go\n");
        writer.StoreAsync().get();
        writer.DetachStream();
        input.Close();
    }

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(WAIT_TIMEOUT), !state->releaseReturned.wait(c_completionTimeoutMs), "Final release did not return");

    module.reset();
    THROW_HR_IF_MSG(
        E_UNEXPECTED, GetModuleHandleW(modulePath.c_str()) == nullptr, "SDK unloaded while its IO callback was blocked");

    state->resumeCallback.SetEvent();
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(WAIT_TIMEOUT), !state->callbackReturned.wait(c_completionTimeoutMs), "IO callback did not return");
    THROW_HR_IF_MSG(E_UNEXPECTED, state->callbackTimedOut.load(), "IO callback timed out");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(c_completionTimeoutMs);
    while (GetModuleHandleW(modulePath.c_str()) != nullptr && std::chrono::steady_clock::now() < deadline)
    {
        Sleep(10);
    }

    THROW_HR_IF_MSG(E_UNEXPECTED, GetModuleHandleW(modulePath.c_str()) != nullptr, "SDK remained loaded after cleanup");
}

void Run(const wchar_t* sessionName, const wchar_t* storagePath, const wchar_t* imagePath, std::wstring_view ownerKind)
{
    const auto modulePath = std::filesystem::path(wil::GetModuleFileNameW<std::wstring>()).parent_path() / L"wslcsdk.dll";
    THROW_HR_IF_MSG(E_UNEXPECTED, GetModuleHandleW(modulePath.c_str()) != nullptr, "SDK was already loaded before the test");
    wil::unique_hmodule module{LoadLibraryW(modulePath.c_str())};
    THROW_LAST_ERROR_IF_NULL(module);

    WSLCSDK::Process process{nullptr};
    winrt::Windows::Foundation::IUnknown owner{nullptr};
    wil::com_ptr<IWSLCSession> runtimeSession;
    auto terminateSession = wil::scope_exit([&]() {
        if (runtimeSession)
        {
            LOG_IF_FAILED(runtimeSession->Terminate());
        }
    });
    {
        // Explicit factories avoid activation-cache references that would keep the SDK loaded.
        auto settings =
            GetFactory<WSLCSDK::ISessionSettingsFactory>(module.get(), L"Microsoft.WSL.Containers.SessionSettings").CreateInstance(sessionName, storagePath);
        settings.CpuCount(4);
        settings.MemorySizeInMB(2048);
        settings.Timeout(std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(std::chrono::minutes(2)));
        settings.VhdRequirements(GetFactory<WSLCSDK::IVhdOptionsFactory>(module.get(), L"Microsoft.WSL.Containers.VhdOptions")
                                     .CreateInstance(L"", 4096ull * 1024 * 1024, WSLCSDK::VhdType::Dynamic));
        auto session = GetFactory<WSLCSDK::ISessionFactory>(module.get(), L"Microsoft.WSL.Containers.Session").CreateInstance(settings);
        session.Start();

        // The service proxy keeps the VM alive without retaining an SDK object or loader reference.
        wil::com_ptr<IWSLCSessionManager> manager;
        THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(manager.get());
        THROW_IF_FAILED(manager->OpenSessionByName(sessionName, &runtimeSession));
        wsl::windows::common::security::ConfigureForCOMImpersonation(runtimeSession.get());
        session.LoadImage(imagePath);

        auto initSettings =
            GetFactory<winrt::Windows::Foundation::IActivationFactory>(module.get(), L"Microsoft.WSL.Containers.ProcessSettings")
                .ActivateInstance<WSLCSDK::ProcessSettings>();
        initSettings.OutputMode(WSLCSDK::ProcessOutputMode::Event);
        initSettings.EnableStandardInput(true);
        initSettings.CommandLine(winrt::single_threaded_vector<winrt::hstring>(
            ownerKind == L"container" ? std::vector<winrt::hstring>{L"/bin/sh", L"-c", L"read line; echo OUT; echo ERR >&2"}
                                      : std::vector<winrt::hstring>{L"/bin/sleep", L"infinity"}));
        auto containerSettings =
            GetFactory<WSLCSDK::IContainerSettingsFactory>(module.get(), L"Microsoft.WSL.Containers.ContainerSettings")
                .CreateInstance(L"debian:latest");
        containerSettings.InitProcess(initSettings);
        auto created = session.CreateContainer(containerSettings);
        auto container = session.OpenContainer(created.Id(), WSLCSDK::ProcessOutputMode::Event);
        created.Close();
        container.Start();

        if (ownerKind == L"container")
        {
            process = container.InitProcess();
            owner = std::move(container);
        }
        else
        {
            THROW_HR_IF(E_INVALIDARG, ownerKind != L"process");
            auto processSettings = GetFactory<winrt::Windows::Foundation::IActivationFactory>(
                                       module.get(), L"Microsoft.WSL.Containers.ProcessSettings")
                                       .ActivateInstance<WSLCSDK::ProcessSettings>();
            processSettings.CommandLine(
                winrt::single_threaded_vector<winrt::hstring>({L"/bin/sh", L"-c", L"read line; echo OUT; echo ERR >&2"}));
            processSettings.OutputMode(WSLCSDK::ProcessOutputMode::Event);
            processSettings.EnableStandardInput(true);
            process = container.CreateProcess(processSettings);
            process.Start();
            owner = process;
            container.Close();
        }

        session.Close();
    }

    VerifyUnload(std::move(module), modulePath, std::move(process), std::move(owner));
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try
    {
        THROW_HR_IF(E_INVALIDARG, argc != 5);
        auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
        WSADATA wsaData{};
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &wsaData));
        auto cleanup = wil::scope_exit([]() { WSACleanup(); });
        Run(argv[1], argv[2], argv[3], argv[4]);
        return 0;
    }
    catch (winrt::hresult_error const& error)
    {
        fwprintf(stderr, L"SDK unload failed: 0x%08x %ls\n", static_cast<unsigned>(error.code()), error.message().c_str());
        return 1;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "SDK unload failed: %s\n", error.what());
        return 1;
    }
}
