/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    main.cpp

Abstract:

    This file contains the entrypoint for wslhost.

--*/

#include "precomp.h"
#include "CommandLine.h"
#include <NotificationActivationCallback.h>
#include <windows.ui.notifications.h>

using namespace ABI::Windows::Data::Xml::Dom;
using namespace ABI::Windows::UI::Notifications;
using namespace Microsoft::WRL;
using namespace wsl::windows::common;
using namespace wsl::shared;

namespace {

// Event used to signal that the COM server should exit.
wil::unique_event g_exitEvent;

void AddComRef()
{
    CoAddRefServerProcess();
}

void ReleaseComRef()
{
    if (CoReleaseServerProcess() == 0)
    {
        g_exitEvent.SetEvent();
    }
}

void ShellExec(_In_ LPCWSTR operation, _In_ LPCWSTR file, _In_ LPCWSTR args)
{
    THROW_LAST_ERROR_IF(reinterpret_cast<intptr_t>(::ShellExecuteW(nullptr, operation, file, args, nullptr, SW_SHOW)) < 32);
}

void LaunchWsl(_In_ LPCWSTR args)
{
    const auto path = wsl::windows::common::wslutil::GetBasePath() / L"wsl.exe";
    ShellExec(L"runas", path.c_str(), args);
}

} // namespace

class DECLSPEC_UUID("2B9C59C3-98F1-45C8-B87B-12AE3C7927E8") NotificationActivator
    : public winrt::implements<NotificationActivator, INotificationActivationCallback>
{
public:
    NotificationActivator()
    {
        AddComRef();
    }

    ~NotificationActivator() override
    {
        ReleaseComRef();
    }

    STDMETHODIMP Activate(_In_ LPCWSTR appUserModelId, _In_ LPCWSTR invokedArgs, _In_reads_(dataCount) const NOTIFICATION_USER_INPUT_DATA* data, ULONG dataCount) noexcept override
    try
    {
        // Log telemetry when a WSL notification is activated, used to determine user engagement for notifications
        WSL_LOG_TELEMETRY("NotificationActivate", PDT_ProductAndServicePerformance, TraceLoggingValue(invokedArgs, "Arguments"));

        ArgumentParser parser(invokedArgs, wslhost::binary_name, 0);
        parser.AddArgument(
            []() {
                std::wstring path;
                THROW_IF_FAILED(wil::GetSystemDirectoryW(path));

                ShellExec(L"runas", (std::filesystem::path(std::move(path)) / L"eventvwr.exe").c_str(), L"/c:Application");
            },
            wslhost::event_viewer_arg);

        parser.AddArgument([]() { ShellExec(nullptr, L"https://github.com/microsoft/WSL/releases", nullptr); }, wslhost::release_notes_arg);

        parser.AddArgument([](auto) { LaunchWsl(WSL_UPDATE_ARG); }, wslhost::update_arg);

        parser.AddArgument(
            [](auto) {
                LaunchWsl(std::format(L"{} {} {}", WSL_INSTALL_ARG, WSL_INSTALL_ARG_NO_DISTRIBUTION_OPTION, WSL_INSTALL_ARG_PROMPT_BEFORE_EXIT_OPTION)
                              .c_str());
            },
            wslhost::install_prerequisites_arg);

        parser.AddArgument(
            [](LPCWSTR input) {
                if (wsl::shared::string::IsEqual(input, wslhost::docs_arg_filesystem_url, false))
                {
                    ShellExec(nullptr, wslhost::docs_arg_filesystem_url, nullptr);
                }
                else
                {
                    THROW_HR_MSG(E_INVALIDARG, "Unexpected docs arg: %ls", input);
                }
            },
            wslhost::docs_arg);

        parser.AddArgument(
            [](LPCWSTR input) {
                if (wsl::shared::string::IsEqual(input, LXSS_NOTIFICATION_DRVFS_PERF_DISABLED, false))
                {
                    const auto lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
                    wsl::windows::common::registry::WriteDword(lxssKey.get(), LXSS_NOTIFICATIONS_KEY, LXSS_NOTIFICATION_DRVFS_PERF_DISABLED, 1);
                }
                else
                {
                    THROW_HR_MSG(E_INVALIDARG, "Unexpected notification arg: %ls", input);
                }
            },
            wslhost::disable_notification_arg);

        parser.Parse();

        return S_OK;
    }
    CATCH_RETURN()
};

class NotificationActivatorFactory : public winrt::implements<NotificationActivatorFactory, IClassFactory>
{
public:
    STDMETHODIMP CreateInstance(_In_ IUnknown* outer, REFIID iid, _COM_Outptr_ void** result) noexcept override
    try
    {
        *result = nullptr;
        THROW_HR_IF(CLASS_E_NOAGGREGATION, outer != nullptr);

        return winrt::make<NotificationActivator>()->QueryInterface(iid, result);
    }
    CATCH_RETURN()

    STDMETHODIMP LockServer(BOOL lock) noexcept override
    {
        if (lock)
        {
            AddComRef();
        }
        else
        {
            ReleaseComRef();
        }

        return S_OK;
    }
};

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
try
{
    wsl::windows::common::wslutil::ConfigureCrt();
    wsl::windows::common::wslutil::InitializeWil();

    // Initialize logging.
    WslTraceLoggingInitialize(LxssTelemetryProvider, !wsl::shared::OfficialBuild);
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [] { WslTraceLoggingUninitialize(); });

    // Initialize COM.
    auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    wsl::windows::common::wslutil::CoInitializeSecurity();

    // Initialize winsock.
    WSADATA data;
    THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &data));

    // Parse arguments.
    wil::unique_handle event{};
    GUID distroId{GUID_NULL};
    wil::unique_handle handle{};
    wil::unique_handle parent{};
    GUID vmId{GUID_NULL};
    wil::unique_com_class_object_cookie cookie;

    wsl::shared::ArgumentParser parser(GetCommandLineW(), wslhost::binary_name);
    parser.AddArgument(distroId, wslhost::distro_id_option);
    parser.AddArgument(Handle(handle), wslhost::handle_option);
    parser.AddArgument(Handle(event), wslhost::event_option);
    parser.AddArgument(Handle(parent), wslhost::parent_option);
    parser.AddArgument(vmId, wslhost::vm_id_option);
    parser.AddArgument(
        [&](auto) {
            // Create an event to be signaled when the last COM object is released.
            g_exitEvent = wil::unique_event(wil::EventOptions::ManualReset);

            THROW_IF_FAILED(::CoRegisterClassObject(
                __uuidof(NotificationActivator), winrt::make<NotificationActivatorFactory>().get(), CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &cookie));

            return 0;
        },
        wslhost::embedding_option);

    parser.Parse();

    if (cookie)
    {
        // Wait until all objects have been released.
        g_exitEvent.wait();

        return 0;
    }

    WI_ASSERT(GetCurrentPackageId(nullptr, nullptr) != ERROR_SUCCESS);

    // Launch the interop server.
    //
    // See GitHub #7568. There needs to be a console for interop.
    // From GitHub #8161 we learned we can't be attached to the same
    // console as wsl.exe. If we are we will be terminated and unable
    // to serve daemonized processes after the console is closed.
    wsl::windows::common::helpers::CreateConsole(nullptr);

    // Register this process with the instance's lifetime management.
    auto service = wil::CoCreateInstance<LxssUserSession, ILxssUserSession>(CLSCTX_LOCAL_SERVER);
    if (!IsEqualGUID(distroId, GUID_NULL))
    {
        ClientExecutionContext context(false);

        service->CreateInstance(
            &distroId, (LXSS_CREATE_INSTANCE_FLAGS_ALLOW_FS_UPGRADE | LXSS_CREATE_INSTANCE_FLAGS_OPEN_EXISTING), context.OutError());
    }

    // Signal the registration complete event if one was supplied.
    if (event)
    {
        THROW_IF_WIN32_BOOL_FALSE(SetEvent(event.get()));
    }

    // If a parent process handle was supplied, wait for the parent
    // process to exit before starting the worker loop.
    if (parent)
    {
        WaitForSingleObject(parent.get(), INFINITE);
    }

    // Begin handling interop requests.
    if (IsEqualGUID(vmId, GUID_NULL))
    {
        wsl::windows::common::interop::WorkerThread(std::move(handle));
    }
    else
    {
        wsl::shared::SocketChannel channel{wil::unique_socket{reinterpret_cast<SOCKET>(handle.release())}, "Interop-wslhost"};

        // This is required because there could have been messages between the process and wsl.exe, and wslhost has no way to know what the sequence numbers were.
        channel.IgnoreSequenceNumbers();

        wsl::windows::common::interop::VmModeWorkerThread(channel, vmId, true);
    }

    return 0;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return 1;
}
