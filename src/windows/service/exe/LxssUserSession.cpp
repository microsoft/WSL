/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssUserSession.cpp

Abstract:

    This file contains session function definitions.

--*/

#include "precomp.h"
#include "Localization.h"
#include "LxssUserSession.h"
#include "LxssInstance.h"
#include "LxssSecurity.h"
#include "WslCoreInstance.h"
#include "resource.h"
#include <winrt\Windows.ApplicationModel.Background.h>
#include <nlohmann\json.hpp>

// Registry keys for migrating legacy distro user config.
#define LXSS_LEGACY_APPEND_NT_PATH L"AppendNtPath"
#define LXSS_LEGACY_INTEROP_ENABLED L"InteropEnabled"

#define LXSS_BSDTAR_PATH LXSS_TOOLS_MOUNT "/bsdtar"
#define LXSS_BSDTAR_CREATE_ARGS " -c --one-file-system --xattrs -f - ."
#define LXSS_BSDTAR_CREATE_ARGS_GZIP " -cz --one-file-system --xattrs -f - ."
#define LXSS_BSDTAR_CREATE_ARGS_XZIP " -cJ --one-file-system --xattrs -f - ."
#define LXSS_BSDTAR_EXTRACT_ARGS " -x -p --xattrs --no-acls -f -"
#define LXSS_ROOTFS_MOUNT "/rootfs"
#define LXSS_TOOLS_MOUNT "/tools"

constexpr auto c_shortIconName = L"shortcut.ico";

// 16 MB buffer used for relaying tar contents via hvsocket.
#define LXSS_RELAY_BUFFER_SIZE (0x1000000)

extern bool g_lxcoreInitialized;

using namespace std::placeholders;
using namespace Microsoft::WRL;
using namespace wsl::windows::service;
using wsl::windows::common::Context;
using wsl::windows::common::ExecutionContext;
using wsl::windows::common::ServiceExecutionContext;

LxssUserSession::LxssUserSession(_In_ const std::weak_ptr<LxssUserSessionImpl>& Session) : m_session(Session)
{
    return;
}

HRESULT STDMETHODCALLTYPE LxssUserSession::ConfigureDistribution(_In_opt_ LPCGUID DistroGuid, _In_ ULONG DefaultUid, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->ConfigureDistribution(DistroGuid, DefaultUid, Flags);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::AttachDisk(_In_ LPCWSTR Disk, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    if constexpr (wsl::shared::Arm64)
    {
        // Pass-through disk support for ARM64 was added to Windows version 27653.
        if (wsl::windows::common::helpers::GetWindowsVersion().BuildNumber < 27653)
        {
            return WSL_E_WSL_MOUNT_NOT_SUPPORTED;
        }
    }

    RETURN_HR_IF(
        WSL_E_DISK_MOUNT_DISABLED,
        !wsl::windows::policies::IsFeatureAllowed(wsl::windows::policies::OpenPoliciesKey().get(), wsl::windows::policies::c_allowDiskMount));

    RETURN_HR_IF(
        E_INVALIDARG,
        ((WI_IsFlagSet(Flags, LXSS_ATTACH_MOUNT_FLAGS_VHD) && WI_IsFlagSet(Flags, LXSS_ATTACH_MOUNT_FLAGS_PASS_THROUGH)) ||
         (WI_IsAnyFlagSet(Flags, ~(LXSS_ATTACH_MOUNT_FLAGS_VHD | LXSS_ATTACH_MOUNT_FLAGS_PASS_THROUGH)))));

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->AttachDisk(Disk, Flags);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::CreateInstance(_In_opt_ LPCGUID DistroGuid, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->CreateInstance(DistroGuid, Flags);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::CreateInstance(_In_ LPCWSTR DistributionName, _In_ ULONG Flags)
try
{
    RETURN_HR_IF(E_INVALIDARG, WI_IsAnyFlagSet(Flags, ~WslSupportCreateInstanceFlags::IgnoreClient));

    GUID distroGuid{};
    RETURN_IF_FAILED(GetDistributionId(DistributionName, 0, nullptr, &distroGuid));

    ULONG internalFlags = 0;
    WI_SetFlagIf(internalFlags, LXSS_CREATE_INSTANCE_FLAGS_IGNORE_CLIENT, WI_IsFlagSet(Flags, WslSupportCreateInstanceFlags::IgnoreClient));

    return CreateInstance(&distroGuid, internalFlags, nullptr);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::CreateLxProcess(
    _In_opt_ LPCGUID DistroGuid,
    _In_opt_ LPCSTR Filename,
    _In_ ULONG CommandLineCount,
    _In_reads_opt_(CommandLineCount) LPCSTR* CommandLine,
    _In_opt_ LPCWSTR CurrentWorkingDirectory,
    _In_opt_ LPCWSTR NtPath,
    _In_reads_opt_(NtEnvironmentLength) PWCHAR NtEnvironment,
    _In_ ULONG NtEnvironmentLength,
    _In_opt_ LPCWSTR Username,
    _In_ SHORT Columns,
    _In_ SHORT Rows,
    _In_ ULONG ConsoleHandle,
    _In_ PLXSS_STD_HANDLES StdHandles,
    _In_ ULONG Flags,
    _Out_ GUID* DistributionId,
    _Out_ GUID* InstanceId,
    _Out_ HANDLE* ProcessHandle,
    _Out_ HANDLE* ServerHandle,
    _Out_ HANDLE* StandardIn,
    _Out_ HANDLE* StandardOut,
    _Out_ HANDLE* StandardErr,
    _Out_ HANDLE* CommunicationChannel,
    _Out_ HANDLE* InteropSocket,
    _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->CreateLxProcess(
        DistroGuid,
        Filename,
        CommandLineCount,
        CommandLine,
        CurrentWorkingDirectory,
        NtPath,
        NtEnvironment,
        NtEnvironmentLength,
        Username,
        Columns,
        Rows,
        ULongToHandle(ConsoleHandle),
        StdHandles,
        Flags,
        DistributionId,
        InstanceId,
        ProcessHandle,
        ServerHandle,
        StandardIn,
        StandardOut,
        StandardErr,
        CommunicationChannel,
        InteropSocket);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::DetachDisk(_In_ LPCWSTR Disk, _Out_ int* Result, _Out_ int* Step, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->DetachDisk(Disk, Result, Step);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::EnumerateDistributions(
    _Out_ PULONG DistributionCount, _Out_ LXSS_ENUMERATE_INFO** Distributions, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->EnumerateDistributions(DistributionCount, Distributions);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::ExportDistribution(
    _In_opt_ LPCGUID DistroGuid, _In_ HANDLE FileHandle, _In_ HANDLE ErrorHandle, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->ExportDistribution(DistroGuid, FileHandle, ErrorHandle, Flags);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::ExportDistributionPipe(
    _In_opt_ LPCGUID DistroGuid, _In_ HANDLE PipeHandle, _In_ HANDLE ErrorHandle, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->ExportDistribution(DistroGuid, PipeHandle, ErrorHandle, Flags);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::GetDefaultDistribution(_Out_ LXSS_ERROR_INFO* Error, _Out_ LPGUID DefaultDistribution)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->GetDefaultDistribution(DefaultDistribution);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::GetDistributionConfiguration(
    _In_opt_ LPCGUID DistroGuid,
    _Out_ LPWSTR* DistributionName,
    _Out_ ULONG* Version,
    _Out_ ULONG* DefaultUid,
    _Out_ ULONG* DefaultEnvironmentCount,
    _Out_ LPSTR** DefaultEnvironment,
    _Out_ ULONG* Flags,
    _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->GetDistributionConfiguration(
        DistroGuid, DistributionName, Version, DefaultUid, DefaultEnvironmentCount, DefaultEnvironment, Flags);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::GetDistributionConfiguration(
    _In_ LPCWSTR DistributionName,
    _Out_ ULONG* Version,
    _Out_ ULONG* DefaultUid,
    _Out_ ULONG* DefaultEnvironmentCount,
    _Out_ LPSTR** DefaultEnvironment,
    _Out_ ULONG* WslFlags)
try
{
    GUID distroGuid{};
    RETURN_IF_FAILED(GetDistributionId(DistributionName, 0, nullptr, &distroGuid));

    wil::unique_cotaskmem_string distroNameLocal;
    const auto result = GetDistributionConfiguration(
        &distroGuid, &distroNameLocal, Version, DefaultUid, DefaultEnvironmentCount, DefaultEnvironment, WslFlags, nullptr);

    WI_ASSERT(FAILED(result) || wsl::shared::string::IsEqual(DistributionName, distroNameLocal.get(), true));

    return result;
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::GetDistributionId(_In_ LPCWSTR DistributionName, _In_ ULONG Flags, _Out_ LXSS_ERROR_INFO* Error, _Out_ GUID* pDistroGuid)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->GetDistributionId(DistributionName, Flags, pDistroGuid);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::ImportDistributionInplace(
    _In_ LPCWSTR DistributionName, _In_ LPCWSTR VhdPath, _Out_ LXSS_ERROR_INFO* Error, _Out_ GUID* pDistroGuid)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->ImportDistributionInplace(DistributionName, VhdPath, pDistroGuid);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::ListDistributions(_Out_ ULONG* Count, _Out_ LPWSTR** Distributions)
try
{

    wil::unique_cotaskmem_array_ptr<LXSS_ENUMERATE_INFO> distributions;
    RETURN_IF_FAILED(EnumerateDistributions(distributions.size_address<ULONG>(), &distributions, nullptr));

    // Filter out distributions that are not in the installed or running state.
    std::vector<wil::unique_cotaskmem_string> installedDistros{};
    for (size_t index = 0; index < distributions.size(); index += 1)
    {
        if ((distributions[index].State == LxssDistributionStateInstalled) || (distributions[index].State == LxssDistributionStateRunning))
        {
            installedDistros.emplace_back(wil::make_unique_string<wil::unique_cotaskmem_string>(distributions[index].DistroName));
        }
    }

    auto userDistributions(wil::make_unique_cotaskmem<LPWSTR[]>(installedDistros.size()));
    for (size_t index = 0; index < installedDistros.size(); index += 1)
    {
        userDistributions.get()[index] = installedDistros[index].release();
    }

    *Count = gsl::narrow_cast<ULONG>(installedDistros.size());
    *Distributions = userDistributions.release();
    return S_OK;
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::MountDisk(
    _In_ LPCWSTR Disk,
    _In_ ULONG Flags,
    _In_ ULONG PartitionIndex,
    _In_opt_ LPCWSTR Name,
    _In_opt_ LPCWSTR Type,
    _In_opt_ LPCWSTR Options,
    _Out_ int* Result,
    _Out_ int* Step,
    _Out_ LPWSTR* MountName,
    _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->MountDisk(Disk, Flags, PartitionIndex, Name, Type, Options, Result, Step, MountName);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::MoveDistribution(_In_ LPCGUID DistroGuid, _In_ LPCWSTR Location, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->MoveDistribution(DistroGuid, Location);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::RegisterDistribution(
    _In_ LPCWSTR DistributionName,
    _In_ ULONG Version,
    _In_ HANDLE FileHandle,
    _In_ HANDLE ErrorHandle,
    _In_ LPCWSTR TargetDirectory,
    _In_ ULONG Flags,
    _In_ ULONG64 VhdSize,
    _In_opt_ LPCWSTR PackageFamilyName,
    _Out_ LPWSTR* InstalledDistributionName,
    _Out_ LXSS_ERROR_INFO* Error,
    _Out_ GUID* pDistroGuid)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->RegisterDistribution(
        DistributionName, Version, FileHandle, ErrorHandle, TargetDirectory, Flags, VhdSize, PackageFamilyName, InstalledDistributionName, pDistroGuid);
}
CATCH_RETURN()

HRESULT LxssUserSession::RegisterDistribution(
    _In_ LPCWSTR DistributionName, _In_ ULONG Version, _In_opt_ HANDLE TarGzFile, _In_opt_ HANDLE TarGzPipe, _In_ LPCWSTR TargetDirectory)
try
{
    const auto clientProcess = wsl::windows::common::wslutil::OpenCallingProcess(PROCESS_QUERY_LIMITED_INFORMATION);
    const auto packageFamilyName = wsl::windows::common::wslutil::GetPackageFamilyName(clientProcess.get());
    GUID distroGuid{};

    return RegisterDistribution(
        DistributionName,
        Version,
        TarGzFile,
        nullptr,
        TargetDirectory,
        0,
        0,
        packageFamilyName.empty() ? nullptr : packageFamilyName.c_str(),
        nullptr,
        nullptr,
        &distroGuid);
}
CATCH_RETURN();

HRESULT STDMETHODCALLTYPE LxssUserSession::RegisterDistributionPipe(
    _In_ LPCWSTR DistributionName,
    _In_ ULONG Version,
    _In_ HANDLE PipeHandle,
    _In_ HANDLE ErrorHandle,
    _In_ LPCWSTR TargetDirectory,
    _In_ ULONG Flags,
    _In_ ULONG64 VhdSize,
    _In_opt_ LPCWSTR PackageFamilyName,
    _Out_ LPWSTR* InstalledDistributionName,
    _Out_ LXSS_ERROR_INFO* Error,
    _Out_ GUID* pDistroGuid)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->RegisterDistribution(
        DistributionName, Version, PipeHandle, ErrorHandle, TargetDirectory, Flags, VhdSize, PackageFamilyName, InstalledDistributionName, pDistroGuid);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::SetDefaultDistribution(_In_ LPCGUID DistroGuid, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->SetDefaultDistribution(DistroGuid);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::SetDistributionConfiguration(_In_ LPCWSTR DistributionName, _In_ ULONG DefaultUid, _In_ ULONG WslFlags)
try
{
    GUID distroGuid{};
    RETURN_IF_FAILED(GetDistributionId(DistributionName, 0, nullptr, &distroGuid));

    return ConfigureDistribution(&distroGuid, DefaultUid, WslFlags, nullptr);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::SetSparse(_In_ LPCGUID DistroGuid, _In_ BOOLEAN Sparse, _In_ BOOLEAN AllowUnsafe, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->SetSparse(DistroGuid, Sparse, AllowUnsafe);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::ResizeDistribution(_In_ LPCGUID DistroGuid, _In_ HANDLE OutputHandle, _In_ ULONG64 NewSize, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->ResizeDistribution(DistroGuid, OutputHandle, NewSize);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::SetVersion(_In_ LPCGUID DistroGuid, _In_ ULONG Version, _In_ HANDLE StdErrHandle, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->SetVersion(DistroGuid, Version, StdErrHandle);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::Shutdown(BOOL Force)
try
{
    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->Shutdown(false, Force);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::Shutdown()
try
{
    return Shutdown(false);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::TerminateDistribution(_In_opt_ LPCGUID DistroGuid, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->TerminateDistribution(DistroGuid);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::UnregisterDistribution(_In_ LPCGUID DistroGuid, _Out_ LXSS_ERROR_INFO* Error)
try
{
    ServiceExecutionContext context(Error);

    const auto session = m_session.lock();
    RETURN_HR_IF(RPC_E_DISCONNECTED, !session);

    return session->UnregisterDistribution(DistroGuid);
}
CATCH_RETURN()

HRESULT STDMETHODCALLTYPE LxssUserSession::UnregisterDistribution(_In_ LPCWSTR DistributionName)
try
{
    GUID distroGuid;
    RETURN_IF_FAILED(GetDistributionId(DistributionName, 0, nullptr, &distroGuid));

    return UnregisterDistribution(&distroGuid, nullptr);
}
CATCH_RETURN()

LxssUserSessionImpl::LxssUserSessionImpl(_In_ PSID userSid, _In_ DWORD sessionId, _Inout_ wsl::windows::service::PluginManager& pluginManager) :
    m_sessionId(sessionId), m_pluginManager(pluginManager)
{
    THROW_IF_WIN32_BOOL_FALSE(::CopySid(sizeof(m_userSid), &m_userSid.Sid, userSid));

    try
    {
        wil::unique_hkey lxssKey;
        wil::unique_handle userToken;
        {
            auto runAsUser = wil::CoImpersonateClient();
            userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
            lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
        }

        static std::atomic<DWORD> sessionCookie;

        m_session = {sessionCookie++, nullptr, &m_userSid.Sid};

        // Detect existing legacy installs and convert them to the new format.
        const DWORD state =
            wsl::windows::common::registry::ReadDword(lxssKey.get(), nullptr, LXSS_LEGACY_INSTALL_VALUE, LxssDistributionStateInvalid);

        if (state == LxssDistributionStateInstalled)
        {
            // Create a registration for legacy installs and delete legacy
            // installed state.
            std::lock_guard lock(m_instanceLock);
            _CreateLegacyRegistration(lxssKey.get(), userToken.get());
            wsl::windows::common::registry::DeleteKeyValue(lxssKey.get(), LXSS_LEGACY_INSTALL_VALUE);
        }

        // Create a threadpool timer to terminate a Linux utility VM that is idle.
        m_vmTerminationTimer.reset(CreateThreadpoolTimer(s_VmIdleTerminate, this, nullptr));
        THROW_IF_NULL_ALLOC(m_vmTerminationTimer);

        // Register for timezone update notifications.

        auto listenForTimeZoneChanges = [this] {
            try
            {
                WNDCLASSEX windowClass{};
                windowClass.cbSize = sizeof(windowClass);
                windowClass.lpfnWndProc = s_TimezoneWindowProc;
                windowClass.hInstance = nullptr;
                windowClass.lpszClassName = L"wslservice-timezone-notifications";
                THROW_LAST_ERROR_IF(RegisterClassExW(&windowClass) == 0);

                // Note: HWND_MESSAGE cannot be used here because such windows don't receive broadcast messages like WM_TIMECHANGE
                const wil::unique_hwnd windowHandle{CreateWindowExW(
                    0,
                    windowClass.lpszClassName,
                    nullptr,
                    WS_OVERLAPPEDWINDOW,
                    CW_USEDEFAULT,
                    CW_USEDEFAULT,
                    CW_USEDEFAULT,
                    CW_USEDEFAULT,
                    nullptr,
                    nullptr,
                    windowClass.hInstance,
                    nullptr)};

                THROW_LAST_ERROR_IF(!windowHandle);
                SetWindowLongPtr(windowHandle.get(), GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

                MSG windowMessage{};
                while (GetMessageW(&windowMessage, nullptr, 0, 0))
                {
                    TranslateMessage(&windowMessage);
                    DispatchMessage(&windowMessage);
                }
            }
            CATCH_LOG();
        };

        m_timezoneThread = std::thread{listenForTimeZoneChanges};

        // Shutdown the inbox session for the current user if needed, this is only required once after the
        // lifted package is installed to ensure that the inbox service has released per-user resources.
        LOG_IF_FAILED(wil::ResultFromException(WI_DIAGNOSTICS_INFO, [&] {
            // Open a handle to the service control manager and check if the inbox service is registered.
            const wil::unique_schandle manager{OpenSCManager(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE)};
            THROW_LAST_ERROR_IF(!manager);

            const wil::unique_schandle service{OpenServiceW(manager.get(), LXSS_INBOX_SERVICE_NAME, SERVICE_QUERY_STATUS)};
            if (!service)
            {
                return;
            }

            // Check if the service is already stopped.
            SERVICE_STATUS status;
            THROW_IF_WIN32_BOOL_FALSE(QueryServiceStatus(service.get(), &status));

            if (status.dwCurrentState == SERVICE_STOPPED)
            {
                return;
            }

            // Shutdown the user's session.
            auto runAsUser = wil::impersonate_token(userToken.get());
            const auto wslSupport = wil::CoCreateInstance<LxssUserSessionInBox, IWslSupport>(CLSCTX_LOCAL_SERVER | CLSCTX_ENABLE_CLOAKING);
            THROW_IF_FAILED(wslSupport->Shutdown());
        }));
    }
    CATCH_LOG()
}

LxssUserSessionImpl::~LxssUserSessionImpl()
{
    if (m_timezoneThread.joinable())
    {
        LOG_IF_WIN32_BOOL_FALSE(PostThreadMessage(GetThreadId(m_timezoneThread.native_handle()), WM_QUIT, 0, 0));
        m_timezoneThread.join();
    }

    m_lifetimeManager.ClearCallbacks();

    // Ensure that if there are no running instances.
    WI_ASSERT(m_runningInstances.empty());
}

HRESULT LxssUserSessionImpl::AttachDisk(_In_ LPCWSTR Disk, _In_ ULONG Flags)
{
    ExecutionContext context(Context::AttachDisk);

    std::lock_guard lock(m_instanceLock);
    const wil::unique_hkey lxssKey = s_OpenLxssUserKey();

    // Validate that at least one WSL2 distro is installed
    auto pred = [&](const auto& e) { return WI_IsFlagSet(e.Read(Property::Flags), LXSS_DISTRO_FLAGS_VM_MODE); };

    auto distributions = _EnumerateDistributions(lxssKey.get(), true);
    RETURN_HR_IF(WSL_E_WSL2_NEEDED, !std::any_of(distributions.begin(), distributions.end(), pred));

    return wil::ResultFromException([&]() {
        _CreateVm();
        const auto diskType = WI_IsFlagSet(Flags, LXSS_ATTACH_MOUNT_FLAGS_VHD) ? WslCoreVm::DiskType::VHD : WslCoreVm::DiskType::PassThrough;
        const auto userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
        m_utilityVm->AttachDisk(Disk, diskType, {}, true, userToken.get());
    });
}

HRESULT LxssUserSessionImpl::ConfigureDistribution(_In_opt_ LPCGUID DistroGuid, _In_ ULONG DefaultUid, _In_ ULONG Flags)
try
{
    ExecutionContext context(Context::ConfigureDistro);

    WSL_LOG("ConfigureDistribution", TraceLoggingValue(DefaultUid, "DefaultUid"), TraceLoggingValue(Flags, "Flags"));

    const wil::unique_hkey lxssKey = s_OpenLxssUserKey();
    std::lock_guard lock(m_instanceLock);

    // Ensure the distribution exists.
    auto distribution = DistributionRegistration::OpenOrDefault(lxssKey.get(), DistroGuid);

    auto configuration = s_GetDistributionConfiguration(distribution);

    // Validate parameters.
    RETURN_HR_IF(
        E_INVALIDARG,
        ((DefaultUid == LX_UID_INVALID) || (Flags != LXSS_DISTRO_FLAGS_UNCHANGED && WI_IsAnyFlagSet(Flags, ~LXSS_DISTRO_FLAGS_ALL))));

    // If the configuration is changed, terminate the distribuiton so the new settings will take effect.
    bool modified = false;
    if (DefaultUid != distribution.Read(Property::DefaultUid))
    {
        distribution.Write(Property::DefaultUid, DefaultUid);
        modified = true;
    }

    if (Flags != LXSS_DISTRO_FLAGS_UNCHANGED)
    {
        // The VM Mode flag is not configurable via this API.
        if (WI_IsFlagSet(configuration.Flags, LXSS_DISTRO_FLAGS_VM_MODE))
        {
            WI_SetFlag(Flags, LXSS_DISTRO_FLAGS_VM_MODE);
        }
        else
        {
            WI_ClearFlag(Flags, LXSS_DISTRO_FLAGS_VM_MODE);
        }

        if (Flags != configuration.Flags)
        {
            distribution.Write(Property::Flags, Flags);
            modified = true;
        }
    }

    if (modified)
    {
        _TerminateInstanceInternal(&distribution.Id(), false);
    }

    return S_OK;
}
CATCH_RETURN()

HRESULT LxssUserSessionImpl::CreateInstance(_In_opt_ LPCGUID DistroGuid, _In_ ULONG Flags)
try
{
    // Register the client process with the lifetime manager so when the last
    // client goes away the instance is terminated (after a timeout).
    _CreateInstance(DistroGuid, Flags);
    return S_OK;
}
CATCH_RETURN()

HRESULT LxssUserSessionImpl::CreateLxProcess(
    _In_opt_ LPCGUID DistroGuid,
    _In_opt_ LPCSTR Filename,
    _In_ ULONG CommandLineCount,
    _In_reads_opt_(CommandLineCount) LPCSTR* CommandLine,
    _In_opt_ LPCWSTR CurrentWorkingDirectory,
    _In_opt_ LPCWSTR NtPath,
    _In_reads_opt_(NtEnvironmentLength) PWCHAR NtEnvironment,
    _In_ ULONG NtEnvironmentLength,
    _In_opt_ LPCWSTR Username,
    _In_ SHORT Columns,
    _In_ SHORT Rows,
    _In_ HANDLE ConsoleHandle,
    _In_ PLXSS_STD_HANDLES StdHandles,
    _In_ ULONG Flags,
    _Out_ GUID* DistributionId,
    _Out_ GUID* InstanceId,
    _Out_ HANDLE* ProcessHandle,
    _Out_ HANDLE* ServerHandle,
    _Out_ HANDLE* StandardIn,
    _Out_ HANDLE* StandardOut,
    _Out_ HANDLE* StandardErr,
    _Out_ HANDLE* CommunicationChannel,
    _Out_ HANDLE* InteropSocket)
try
{
    // This API handles launching processes three ways:
    // 1. If Filename and CommandLine are both NULL, the user's default shell
    //    is launched. The default shell is stored in /etc/passwd.
    // 2. If Filename is NULL but CommandLine is not, the user's default shell
    //    is used to invoke the specified command. For example:
    //        /bin/bash -c "command"
    // 3. If Filename and CommandLine are both non-NULL, they are passed along
    //    as-is to the exec system call by the init daemon.

    // Create an instance to run the process.
    auto instance = _CreateInstance(DistroGuid, Flags);

    // Query process creation context.
    auto distributionId = instance->GetDistributionId();
    auto context = s_GetCreateProcessContext(distributionId, WI_IsFlagSet(Flags, LXSS_CREATE_INSTANCE_FLAGS_USE_SYSTEM_DISTRO));

    _SetHttpProxyInfo(context.DefaultEnvironment);

    // Parse the create process params.
    auto parsed = LxssCreateProcess::ParseArguments(
        Filename,
        CommandLineCount,
        CommandLine,
        CurrentWorkingDirectory,
        NtPath,
        NtEnvironment,
        NtEnvironmentLength,
        Username,
        context.DefaultEnvironment,
        context.Flags);

    if (WI_IsFlagSet(Flags, LXSS_CREATE_INSTANCE_FLAGS_SHELL_LOGIN))
    {
        THROW_HR_IF(E_INVALIDARG, ARGUMENT_PRESENT(Filename));
        parsed.ShellOptions = ShellOptionsLogin;
    }

    // Initialize console data and launch the process.
    CreateLxProcessConsoleData consoleData;
    if (ConsoleHandle)
    {
        consoleData.ConsoleHandle.reset(wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ConsoleHandle));
    }

    consoleData.ClientProcess = wsl::windows::common::wslutil::OpenCallingProcess(PROCESS_VM_READ | GENERIC_READ | SYNCHRONIZE);
    instance->CreateLxProcess(
        parsed, context, consoleData, Columns, Rows, StdHandles, InstanceId, ProcessHandle, ServerHandle, StandardIn, StandardOut, StandardErr, CommunicationChannel, InteropSocket);

    *DistributionId = distributionId;
    return S_OK;
}
CATCH_RETURN()

void LxssUserSessionImpl::ClearDiskStateInRegistry(_In_ const LPCWSTR Disk)
{
    bool deleted = !ARGUMENT_PRESENT(Disk);

    const auto key = wsl::windows::common::registry::OpenOrCreateLxssDiskMountsKey(&m_userSid.Sid);
    for (const auto& e : wsl::windows::common::registry::EnumKeys(key.get(), KEY_READ))
    {
        if (Disk == nullptr || wsl::windows::common::registry::ReadString(e.second.get(), nullptr, c_diskValueName) == Disk)
        {
            wsl::windows::common::registry::DeleteKey(key.get(), e.first.c_str());
            deleted = true;
        }
    }

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !deleted);
}

HRESULT LxssUserSessionImpl::DetachDisk(_In_ LPCWSTR Disk, _Out_ int* Result, _Out_ int* Step)
{
    ExecutionContext context(Context::DetachDisk);

    std::lock_guard lock(m_instanceLock);

    // If the UVM isn't running, simply clear the disk state in the registry, if any
    if (!m_utilityVm)
    {
        return wil::ResultFromException([&]() {
            ClearDiskStateInRegistry(Disk);
            *Result = 0;
            *Step = LxMiniInitMountStepNone;
        });
    }

    return wil::ResultFromException([&]() { std::tie(*Result, *Step) = m_utilityVm->DetachDisk(Disk); });
}

HRESULT LxssUserSessionImpl::MountDisk(
    _In_ LPCWSTR Disk,
    _In_ ULONG Flags,
    _In_ ULONG PartitionIndex,
    _In_opt_ LPCWSTR Name,
    _In_opt_ LPCWSTR Type,
    _In_opt_ LPCWSTR Options,
    _Out_ int* Result,
    _Out_ int* Step,
    _Out_ LPWSTR* MountName)
{
    ExecutionContext context(Context::DetachDisk);

    std::lock_guard lock(m_instanceLock);
    return wil::ResultFromException([&]() {
        _CreateVm();
        const auto MountDiskType = WI_IsFlagSet(Flags, LXSS_ATTACH_MOUNT_FLAGS_VHD) ? WslCoreVm::DiskType::VHD : WslCoreVm::DiskType::PassThrough;
        const auto MountResult = m_utilityVm->MountDisk(Disk, MountDiskType, PartitionIndex, Name, Type, Options);
        const auto MountNameWide = wsl::shared::string::MultiByteToWide(MountResult.MountPointName);
        *Result = MountResult.Result;
        *Step = MountResult.Step;
        *MountName = wil::make_unique_string<wil::unique_cotaskmem_string>(MountNameWide.c_str()).release();
    });
}

HRESULT LxssUserSessionImpl::MoveDistribution(_In_ LPCGUID DistroGuid, _In_ LPCWSTR Location)
{
    ExecutionContext context(Context::MoveDistro);

    std::lock_guard lock(m_instanceLock);

    // Fail if the distribution is running.
    RETURN_HR_IF(WSL_E_DISTRO_NOT_STOPPED, m_runningInstances.contains(*DistroGuid));

    // Lookup the distribution configuration
    const auto lxssKey = s_OpenLxssUserKey();
    _ValidateDistributionNameAndPathNotInUse(lxssKey.get(), Location, nullptr);

    auto registration = DistributionRegistration::Open(lxssKey.get(), *DistroGuid);
    auto distro = s_GetDistributionConfiguration(registration);

    RETURN_HR_IF(E_NOTIMPL, WI_IsFlagClear(distro.Flags, LXSS_DISTRO_FLAGS_VM_MODE));

    // Build the final vhd path.
    std::filesystem::path newVhdPath = Location;
    RETURN_HR_IF(E_INVALIDARG, newVhdPath.empty());

    newVhdPath /= distro.VhdFilePath.filename();

    auto impersonate = wil::CoImpersonateClient();

    // Create the distribution base folder
    std::error_code error;
    std::filesystem::create_directories(Location, error);
    if (error.value())
    {
        THROW_WIN32(error.value());
    }

    // Move the VHD to the new location.
    THROW_IF_WIN32_BOOL_FALSE(MoveFileEx(distro.VhdFilePath.c_str(), newVhdPath.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH));

    auto revert = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        THROW_IF_WIN32_BOOL_FALSE(MoveFileEx(
            newVhdPath.c_str(), distro.VhdFilePath.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));

        // Write the location back to the original path in case the second registry write failed. Otherwise this is a no-op.
        registration.Write(Property::BasePath, distro.BasePath.c_str());
    });

    // Update the registry location
    registration.Write(Property::BasePath, Location);
    registration.Write(Property::VhdFileName, newVhdPath.filename().c_str());

    revert.release();

    return S_OK;
}

HRESULT LxssUserSessionImpl::EnumerateDistributions(_Out_ PULONG DistributionCount, _Out_ LXSS_ENUMERATE_INFO** Distributions)
{
    // Get a list of all registered distributions.
    const wil::unique_hkey lxssKey = s_OpenLxssUserKey();
    std::lock_guard lock(m_instanceLock);
    const auto distributions = _EnumerateDistributions(lxssKey.get(), true);

    // Get the default distribution.
    //
    // N.B. It is possible the default to not exist, for example if there is
    //      a single distribution that is being installed.
    GUID defaultGuid = GUID_NULL;
    try
    {
        defaultGuid = _GetDefaultDistro(lxssKey.get());
    }
    CATCH_LOG()

    const ULONG numberOfDistributions = gsl::narrow_cast<ULONG>(distributions.size());
    auto userDistributions(wil::make_unique_cotaskmem<LXSS_ENUMERATE_INFO[]>(numberOfDistributions));

    // Fill in information about each distribution.
    for (ULONG index = 0; index < numberOfDistributions; index += 1)
    {

        auto configuration = s_GetDistributionConfiguration(distributions[index]);
        auto state = static_cast<LxssDistributionState>(configuration.State);

        if (m_runningInstances.contains(distributions[index].Id()))
        {
            state = LxssDistributionStateRunning;
        }
        else if (state == LxssDistributionStateInstalled)
        {
            auto distro = std::find_if(m_lockedDistributions.begin(), m_lockedDistributions.end(), [&](const auto& pair) {
                return IsEqualGUID(distributions[index].Id(), pair.first);
            });

            if (distro != m_lockedDistributions.end())
            {
                state = distro->second;
            }
        }

        const auto current = &userDistributions.get()[index];
        current->DistroGuid = distributions[index].Id();
        current->State = state;
        current->Version = WI_IsFlagSet(configuration.Flags, LXSS_DISTRO_FLAGS_VM_MODE) ? LXSS_WSL_VERSION_2 : LXSS_WSL_VERSION_1;
        current->Flags = 0;
        WI_SetFlagIf(current->Flags, LXSS_ENUMERATE_FLAGS_DEFAULT, IsEqualGUID(distributions[index].Id(), defaultGuid));

        static_assert((RTL_NUMBER_OF(current->DistroName) - 1) == LX_INIT_DISTRO_NAME_MAX);

        memset(current->DistroName, 0, sizeof(current->DistroName));
        wcscpy_s(current->DistroName, RTL_NUMBER_OF(current->DistroName) - 1, configuration.Name.c_str());
    }

    *DistributionCount = numberOfDistributions;
    *Distributions = userDistributions.release();
    return S_OK;
}

HRESULT LxssUserSessionImpl::ExportDistribution(_In_opt_ LPCGUID DistroGuid, _In_ HANDLE FileHandle, _In_ HANDLE ErrorHandle, _In_ ULONG Flags)
{
    RETURN_HR_IF(E_INVALIDARG, (WI_IsAnyFlagSet(Flags, ~LXSS_EXPORT_DISTRO_FLAGS_ALL)));

    LXSS_DISTRO_CONFIGURATION configuration;
    wil::unique_hkey distroKey;
    try
    {
        const wil::unique_hkey lxssKey = s_OpenLxssUserKey();
        std::lock_guard lock(m_instanceLock);

        const auto registration = DistributionRegistration::OpenOrDefault(lxssKey.get(), DistroGuid);

        // Ensure the distribution is installed.
        configuration = s_GetDistributionConfiguration(registration);
        RETURN_HR_IF(E_ILLEGAL_STATE_CHANGE, (configuration.State != LxssDistributionStateInstalled));

        // Exporting a WSL1 distro is not possible if the VHD flag is specified.
        RETURN_HR_IF(WSL_E_WSL2_NEEDED, WI_IsFlagSet(Flags, LXSS_EXPORT_DISTRO_FLAGS_VHD) && WI_IsFlagClear(configuration.Flags, LXSS_DISTRO_FLAGS_VM_MODE));

        // Exporting a WSL1 distro is not possible if the lxcore driver is not present.
        RETURN_HR_IF(WSL_E_WSL1_NOT_SUPPORTED, WI_IsFlagClear(configuration.Flags, LXSS_DISTRO_FLAGS_VM_MODE) && !g_lxcoreInitialized);

        // Add the distribution to the list of converting distributions.
        _ConversionBegin(configuration.DistroId, LxssDistributionStateExporting);
    }
    CATCH_RETURN()

    // Set up a scope exit member to remove the distribution from the converting list.
    auto exportComplete = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { _ConversionComplete(configuration.DistroId); });

    // Log telemetry to track how long exporting the distribution takes.
    WSL_LOG_TELEMETRY(
        "ExportDistributionBegin",
        PDT_ProductAndServicePerformance,
        TraceLoggingValue(configuration.Name.c_str(), "distroName"),
        TraceLoggingValue(Flags, "flags"));

    HRESULT result;
    auto enableExit = wil::scope_exit([&] {
        WSL_LOG_TELEMETRY(
            "ExportDistributionEnd",
            PDT_ProductAndServicePerformance,
            TraceLoggingValue(configuration.Name.c_str(), "distroName"),
            TraceLoggingValue(result, "result"),
            TraceLoggingValue(Flags, "flags"));
    });

    // Export the distribution.
    try
    {
        const wil::unique_handle clientProcess = wsl::windows::common::wslutil::OpenCallingProcess(GENERIC_READ | SYNCHRONIZE);
        if (WI_IsFlagSet(configuration.Flags, LXSS_DISTRO_FLAGS_VM_MODE))
        {
            if (WI_IsFlagSet(Flags, LXSS_EXPORT_DISTRO_FLAGS_VHD))
            {
                const wil::unique_handle userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
                auto runAsUser = wil::impersonate_token(userToken.get());

                // Ensure the target file has the correct file extension.
                std::wstring exportPath;
                THROW_IF_FAILED(wil::GetFinalPathNameByHandleW(FileHandle, exportPath));

                const auto sourceFileExtension = configuration.VhdFilePath.extension().native();
                const auto targetFileExtension = std::filesystem::path(std::move(exportPath)).extension().native();
                if (!wsl::windows::common::string::IsPathComponentEqual(sourceFileExtension, targetFileExtension))
                {
                    THROW_HR_WITH_USER_ERROR(
                        WSL_E_EXPORT_FAILED, wsl::shared::Localization::MessageRequiresFileExtension(sourceFileExtension.c_str()));
                }

                const wil::unique_hfile vhdFile(CreateFileW(
                    configuration.VhdFilePath.c_str(), GENERIC_READ, (FILE_SHARE_READ | FILE_SHARE_DELETE), nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));

                RETURN_LAST_ERROR_IF(!vhdFile);

                wsl::windows::common::relay::InterruptableRelay(vhdFile.get(), FileHandle, clientProcess.get(), LXSS_RELAY_BUFFER_SIZE);
            }
            else
            {
                auto vmContext = _RunUtilityVmSetup(configuration, LxMiniInitMessageExport, Flags);

                wsl::windows::common::relay::ScopedRelay stdErrRelay(
                    wil::unique_handle{reinterpret_cast<HANDLE>(vmContext.errorSocket.release())}, ErrorHandle);

                // Relay the filesystem file contents to the tar.gz handle.
                wsl::windows::common::relay::InterruptableRelay(
                    reinterpret_cast<HANDLE>(vmContext.tarSocket.get()), FileHandle, clientProcess.get(), LXSS_RELAY_BUFFER_SIZE);

                // Wait for the utility VM to finish expanding the tar and ensure that
                // the operation was successful.
                ULONG exitCode = 1;
                vmContext.instance->GetInitPort()->Receive(&exitCode, sizeof(exitCode), clientProcess.get());

                // Flush any pending IO on the error relay before exiting.
                stdErrRelay.Sync();

                THROW_HR_IF(WSL_E_EXPORT_FAILED, (exitCode != 0));
            }
        }
        else
        {
            auto mounts = _CreateSetupMounts(configuration);

            const char* formatArgs = nullptr;

            if (WI_IsFlagSet(Flags, LXSS_EXPORT_DISTRO_FLAGS_GZIP))
            {
                THROW_HR_IF(E_INVALIDARG, WI_IsFlagSet(Flags, LXSS_EXPORT_DISTRO_FLAGS_XZIP));

                formatArgs = LXSS_BSDTAR_CREATE_ARGS_GZIP;
            }
            else if (WI_IsFlagSet(Flags, LXSS_EXPORT_DISTRO_FLAGS_XZIP))
            {
                formatArgs = LXSS_BSDTAR_CREATE_ARGS_XZIP;
            }
            else
            {
                formatArgs = LXSS_BSDTAR_CREATE_ARGS;
            }

            const auto commandLine = std::format("{} -C {}{}", LXSS_BSDTAR_PATH, LXSS_ROOTFS_MOUNT, formatArgs);

            const auto elfContext = _RunElfBinary(
                commandLine.c_str(),
                configuration.BasePath.c_str(),
                clientProcess.get(),
                nullptr,
                FileHandle,
                ErrorHandle,
                mounts.data(),
                static_cast<ULONG>(mounts.size()));

            const auto exitStatus = _GetElfExitStatus(elfContext);
            THROW_HR_IF(WSL_E_EXPORT_FAILED, exitStatus != 0);
        }

        result = S_OK;
    }
    catch (...)
    {
        result = wil::ResultFromCaughtException();
    }

    return result;
}

HRESULT LxssUserSessionImpl::GetDefaultDistribution(_Out_ LPGUID DefaultDistribution)
try
{
    const wil::unique_hkey lxssKey = s_OpenLxssUserKey();
    std::lock_guard lock(m_instanceLock);
    *DefaultDistribution = _GetDefaultDistro(lxssKey.get());
    return S_OK;
}
CATCH_RETURN()

HRESULT LxssUserSessionImpl::GetDistributionConfiguration(
    _In_opt_ LPCGUID DistroGuid,
    _Out_ LPWSTR* DistributionName,
    _Out_ ULONG* Version,
    _Out_ ULONG* DefaultUid,
    _Out_ ULONG* DefaultEnvironmentCount,
    _Out_ LPSTR** DefaultEnvironment,
    _Out_ ULONG* Flags)
try
{
    const wil::unique_hkey lxssKey = s_OpenLxssUserKey();
    std::lock_guard lock(m_instanceLock);

    const auto registration = DistributionRegistration::OpenOrDefault(lxssKey.get(), DistroGuid);
    const auto configuration = s_GetDistributionConfiguration(registration);

    // Write configuration information back to the calling process.
    *DistributionName = wil::make_cotaskmem_string(configuration.Name.c_str()).release();
    *Version = configuration.Version;
    *DefaultUid = registration.Read(Property::DefaultUid);
    *Flags = configuration.Flags;
    const auto defaultEnvironment = registration.Read(Property::DefaultEnvironmnent);
    *DefaultEnvironmentCount = gsl::narrow_cast<ULONG>(defaultEnvironment.size());
    auto environment(wil::make_unique_cotaskmem<LPSTR[]>(defaultEnvironment.size()));
    for (size_t index = 0; index < defaultEnvironment.size(); index += 1)
    {
        environment.get()[index] =
            wil::make_unique_ansistring<wil::unique_cotaskmem_ansistring>(defaultEnvironment[index].c_str()).release();
    }

    *DefaultEnvironment = environment.release();
    return S_OK;
}
CATCH_RETURN()

HRESULT LxssUserSessionImpl::GetDistributionId(_In_ LPCWSTR DistributionName, _In_ ULONG Flags, _Out_ GUID* pDistroGuid)
try
{
    // The client must provide a non-empty string.
    //
    // N.B. COM insures that the name buffer is non-NULL.
    RETURN_HR_IF(E_INVALIDARG, (wcslen(DistributionName) == 0));

    // Validate flags.
    RETURN_HR_IF(E_INVALIDARG, (WI_IsAnyFlagSet(Flags, ~LXSS_GET_DISTRO_ID_LIST_ALL)));

    // Open the user's lxss registry key.
    const wil::unique_hkey lxssKey = s_OpenLxssUserKey();
    const bool listAll = WI_IsFlagSet(Flags, LXSS_GET_DISTRO_ID_LIST_ALL);
    bool distroFound = false;

    // Lock the session and search for a distribution that has a matching name.
    std::lock_guard lock(m_instanceLock);
    const auto distros = _EnumerateDistributions(lxssKey.get(), listAll);
    for (const auto& registration : distros)
    {
        if (wsl::shared::string::IsEqual(DistributionName, registration.Read(Property::Name), true))
        {
            distroFound = true;
            *pDistroGuid = registration.Id();
            break;
        }
    }

    // Return an error if no distribution was found with a matching name.
    RETURN_HR_IF(WSL_E_DISTRO_NOT_FOUND, !distroFound);

    return S_OK;
}
CATCH_RETURN()

DWORD LxssUserSessionImpl::GetSessionCookie() const
{
    return m_session.SessionId;
}

DWORD LxssUserSessionImpl::GetSessionId() const
{
    return m_sessionId;
}

PSID LxssUserSessionImpl::GetUserSid()
{
    return &m_userSid.Sid;
}

HRESULT
LxssUserSessionImpl::ImportDistributionInplace(_In_ LPCWSTR DistributionName, _In_ LPCWSTR VhdPath, _Out_ GUID* pDistroGuid)
{
    ExecutionContext context(Context::RegisterDistro);

    s_ValidateDistroName(DistributionName);

    // Return an error if the path is not absolute or does not end in the .vhdx file extension.
    const std::filesystem::path path{VhdPath};
    RETURN_HR_IF(
        E_INVALIDARG,
        !path.is_absolute() ||
            (!wsl::windows::common::string::IsPathComponentEqual(path.extension().native(), wsl::windows::common::wslutil::c_vhdFileExtension) &&
             !wsl::windows::common::string::IsPathComponentEqual(path.extension().c_str(), wsl::windows::common::wslutil::c_vhdxFileExtension)));

    const wil::unique_hkey lxssKey = s_OpenLxssUserKey();
    std::lock_guard lock(m_instanceLock);

    // Create a registration for the distribution.
    //
    // N.B. Import inplace is always WSL2.
    _ValidateDistributionNameAndPathNotInUse(lxssKey.get(), path.parent_path().c_str(), DistributionName);

    constexpr ULONG flags = LXSS_DISTRO_FLAGS_DEFAULT | LXSS_DISTRO_FLAGS_VM_MODE;
    auto registration = DistributionRegistration::Create(
        lxssKey.get(),
        {},
        DistributionName,
        LXSS_DISTRO_VERSION_CURRENT,
        path.parent_path().c_str(),
        flags,
        LX_UID_ROOT,
        nullptr,
        path.filename().c_str(),
        false);

    auto configuration = s_GetDistributionConfiguration(registration);

    // Declare a scope exit variable to clean up on failure.
    const wil::unique_handle userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        {
            auto runAsUser = wil::impersonate_token(userToken.get());
            _DeleteDistribution(configuration, LXSS_DELETE_DISTRO_FLAGS_UNMOUNT);
        }

        registration.Delete(lxssKey.get());
    });

    const auto vmContext = _RunUtilityVmSetup(configuration, LxMiniInitMessageImportInplace);
    auto* channel = dynamic_cast<WslCoreInstance::WslCorePort*>(vmContext.instance->GetInitPort().get());

    gsl::span<gsl::byte> span;
    const auto& message = channel->GetChannel().ReceiveMessage<LX_MINI_INIT_IMPORT_RESULT>(&span);

    // Process the import result message.
    THROW_HR_IF(WSL_E_IMPORT_FAILED, (message.Result != 0));

    _ProcessImportResultMessage(message, span, lxssKey.get(), configuration, registration);

    // Set the distribution as installed.
    _SetDistributionInstalled(lxssKey.get(), registration.Id());
    cleanup.release();

    _SendDistributionRegisteredEvent(configuration);

    _LaunchOOBEIfNeeded();

    // Log when a distro is imported in place
    WSL_LOG_TELEMETRY(
        "ImportDistributionInplace",
        PDT_ProductAndServiceUsage,
        TraceLoggingValue(DistributionName, "distroName"),
        TraceLoggingValue(path.filename().c_str(), "fileName"));

    *pDistroGuid = registration.Id();
    return S_OK;
}

HRESULT LxssUserSessionImpl::RegisterDistribution(
    _In_ LPCWSTR DistributionName,
    _In_ ULONG Version,
    _In_ HANDLE FileHandle,
    _In_ HANDLE ErrorHandle,
    _In_ LPCWSTR TargetDirectory,
    _In_ ULONG Flags,
    _In_ ULONG64 VhdSize,
    _In_opt_ LPCWSTR PackageFamilyName,
    _Out_opt_ LPWSTR* InstalledDistributionName,
    _Out_ GUID* pDistroGuid)
{
    ExecutionContext context(Context::RegisterDistro);

    RETURN_HR_IF(E_INVALIDARG, (WI_IsAnyFlagSet(Flags, ~LXSS_IMPORT_DISTRO_FLAGS_ALL)));

    // Set up a scope exit member to log registration status.
    HRESULT result = E_FAIL;
    auto registerExit = wil::scope_exit([&] {
        // Log when a distribution registration ends and its result
        WSL_LOG_TELEMETRY(
            "RegisterDistributionEnd",
            PDT_ProductAndServiceUsage,
            TraceLoggingValue(DistributionName, "name"),
            TraceLoggingHexUInt32(result, "result"),
            TraceLoggingValue(Version, "version"),
            TraceLoggingValue(Flags, "flags"));
    });

    try
    {
        // Log when a distribution is being registered in WSL
        WSL_LOG_TELEMETRY(
            "RegisterDistributionBegin",
            PDT_ProductAndServiceUsage,
            TraceLoggingValue(DistributionName, "name"),
            TraceLoggingValue(Version, "version"),
            TraceLoggingValue(Flags, "flags"));

        if (DistributionName != nullptr)
        {
            s_ValidateDistroName(DistributionName);
        }

        // Impersonate the user and open their lxss registry key.
        wil::unique_hkey lxssKey = s_OpenLxssUserKey();

        // Determine the filesystem version. If WslFs is not enabled, downgrade
        // the version.
        ULONG FilesytemVersion = LXSS_DISTRO_VERSION_CURRENT;
        if (wsl::windows::common::registry::ReadDword(lxssKey.get(), nullptr, WSL_NEW_DISTRO_LXFS, 0) != 0)
        {
            if (LXSS_DISTRO_USES_WSL_FS(FilesytemVersion) != FALSE)
            {
                FilesytemVersion = LXSS_DISTRO_VERSION_1;
            }
        }

        // Validate the version number.
        if (Version == LXSS_WSL_VERSION_DEFAULT)
        {
            Version = wsl::windows::common::registry::ReadDword(lxssKey.get(), nullptr, LXSS_WSL_DEFAULT_VERSION, LXSS_WSL_VERSION_2);
        }

        RETURN_HR_IF(E_INVALIDARG, ((Version != LXSS_WSL_VERSION_1) && (Version != LXSS_WSL_VERSION_2)));

        // Registering a WSL1 distro is not possible if any VHD flags are specified.
        RETURN_HR_IF(
            WSL_E_WSL2_NEEDED,
            WI_IsAnyFlagSet(Flags, LXSS_IMPORT_DISTRO_FLAGS_VHD | LXSS_IMPORT_DISTRO_FLAGS_FIXED_VHD) && (Version == LXSS_WSL_VERSION_1));

        // Registering a vhd with the fixed vhd flag is not allowed.
        if (WI_IsFlagSet(Flags, LXSS_IMPORT_DISTRO_FLAGS_VHD))
        {
            RETURN_HR_IF(E_INVALIDARG, WI_IsFlagSet(Flags, LXSS_IMPORT_DISTRO_FLAGS_FIXED_VHD));
        }

        // Registering a distro with a fixed VHD is only allowed if a size is specified.
        RETURN_HR_IF(E_INVALIDARG, VhdSize == 0 && WI_IsFlagSet(Flags, LXSS_IMPORT_DISTRO_FLAGS_FIXED_VHD));

        // Registering a WSL1 distro is not possible if the lxcore driver is not present.
        RETURN_HR_IF(WSL_E_WSL1_NOT_SUPPORTED, (Version == LXSS_WSL_VERSION_1) && !g_lxcoreInitialized);

        DistributionRegistration registration;
        LXSS_DISTRO_CONFIGURATION configuration;
        std::filesystem::path distributionPath;
        wil::unique_handle userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
        auto config = _GetResultantConfig(userToken.get());

        {
            std::lock_guard lock(m_instanceLock);

            // Create a registration for the distribution and determine which version should be used.
            ULONG flags = LXSS_DISTRO_FLAGS_DEFAULT;
            WI_SetFlagIf(flags, LXSS_DISTRO_FLAGS_VM_MODE, (Version == LXSS_WSL_VERSION_2));

            GUID DistributionId{};
            THROW_IF_FAILED(CoCreateGuid(&DistributionId));

            if (TargetDirectory == nullptr)
            {
                distributionPath = config.DefaultDistributionLocation / wsl::shared::string::GuidToString<wchar_t>(DistributionId);
            }
            else
            {
                distributionPath = TargetDirectory;
            }

            TargetDirectory = nullptr; // Make sure this isn't reused later.

            _ValidateDistributionNameAndPathNotInUse(lxssKey.get(), distributionPath.c_str(), DistributionName);

            if (!std::filesystem::exists(distributionPath))
            {
                auto impersonate = wil::CoImpersonateClient();
                wil::CreateDirectoryDeep(distributionPath.c_str());
            }

            // If importing a vhd, determine if it is a .vhd or .vhdx.
            std::wstring vhdName{LXSS_VM_MODE_VHD_NAME};
            if (WI_IsFlagSet(Flags, LXSS_IMPORT_DISTRO_FLAGS_VHD))
            {
                std::wstring pathBuffer;
                THROW_IF_FAILED(wil::GetFinalPathNameByHandleW(FileHandle, pathBuffer));

                std::filesystem::path vhdPath{std::move(pathBuffer)};

                using namespace wsl::windows::common::wslutil;
                if (!wsl::windows::common::string::IsPathComponentEqual(vhdPath.extension().native(), c_vhdFileExtension) &&
                    !wsl::windows::common::string::IsPathComponentEqual(vhdPath.extension().native(), c_vhdxFileExtension))
                {
                    THROW_HR_WITH_USER_ERROR(
                        WSL_E_IMPORT_FAILED, wsl::shared::Localization::MessageRequiresFileExtensions(c_vhdFileExtension, c_vhdxFileExtension));
                }

                vhdName = vhdPath.filename();
            }

            registration = DistributionRegistration::Create(
                lxssKey.get(),
                DistributionId,
                DistributionName,
                FilesytemVersion,
                distributionPath.c_str(),
                flags,
                LX_UID_ROOT,
                PackageFamilyName,
                vhdName.c_str(),
                WI_IsFlagClear(Flags, LXSS_IMPORT_DISTRO_FLAGS_NO_OOBE));

            configuration = s_GetDistributionConfiguration(registration, DistributionName == nullptr);

            // Add the distribution to the list of converting distributions.
            _ConversionBegin(configuration.DistroId, LxssDistributionStateInstalling);
        }

        // Set up a scope exit member to remove the distribution from the converting list.
        auto installComplete = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { _ConversionComplete(configuration.DistroId); });

        // Declare a scope exit variable to clean up on failure.
        ULONG deleteFlags = 0;
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            {
                auto runAsUser = wil::impersonate_token(userToken.get());
                _DeleteDistribution(configuration, deleteFlags);
            }

            registration.Delete(lxssKey.get());
        });

        // Initialize the filesystem.
        wil::unique_handle clientProcess = wsl::windows::common::wslutil::OpenCallingProcess(GENERIC_READ | SYNCHRONIZE);
        if (Version == LXSS_WSL_VERSION_2)
        {
            if (WI_IsFlagSet(Flags, LXSS_IMPORT_DISTRO_FLAGS_VHD))
            {
                auto runAsUser = wil::impersonate_token(userToken.get());
                auto vhdFile = wsl::core::filesystem::CreateFile(
                    configuration.VhdFilePath.c_str(),
                    GENERIC_WRITE,
                    (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
                    CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL,
                    GetUserSid());

                deleteFlags = LXSS_DELETE_DISTRO_FLAGS_VHD;
                wsl::windows::common::relay::InterruptableRelay(FileHandle, vhdFile.get(), clientProcess.get(), LXSS_RELAY_BUFFER_SIZE);
            }
            else
            {
                // Create a vhd to store the root filesystem.
                {
                    auto runAsUser = wil::impersonate_token(userToken.get());
                    if (VhdSize == 0)
                    {
                        VhdSize = config.VhdSizeBytes;
                    }

                    wsl::core::filesystem::CreateVhd(
                        configuration.VhdFilePath.c_str(), VhdSize, GetUserSid(), config.EnableSparseVhd, WI_IsFlagSet(Flags, LXSS_IMPORT_DISTRO_FLAGS_FIXED_VHD));

                    deleteFlags = LXSS_DELETE_DISTRO_FLAGS_VHD;
                }

                // Create a process in the utility VM to expand the tar file from a socket.
                auto vmContext = _RunUtilityVmSetup(configuration, LxMiniInitMessageImport);

                std::optional<wsl::windows::common::relay::ScopedRelay> errorRelay;
                if (ErrorHandle != nullptr)
                {
                    errorRelay.emplace(std::move(vmContext.errorSocket), ErrorHandle);
                }

                // Relay the filesystem file contents to the tar.gz handle.
                // Note: This is done in a separate thread because we can sometimes get stuck while writing the socket if tar exited without reading anything.
                // Note: because the tarsSocket is moved, the relay owns it, meaning it will automatically close it when the relaying thread exits.
                wsl::windows::common::relay::ScopedRelay dataRelay(FileHandle, std::move(vmContext.tarSocket));

                // Wait for the utility VM to finish expanding the tar and ensure that
                // the operation was successful.
                auto* channel = dynamic_cast<WslCoreInstance::WslCorePort*>(vmContext.instance->GetInitPort().get());

                gsl::span<gsl::byte> span;
                const auto& message = channel->GetChannel().ReceiveMessage<LX_MINI_INIT_IMPORT_RESULT>(&span);

                // Flush any pending IO on the error relay before exiting.
                if (errorRelay.has_value())
                {
                    errorRelay->Sync();
                }

                // Process the import result message.
                THROW_HR_IF(WSL_E_IMPORT_FAILED, (message.Result != 0));

                _ProcessImportResultMessage(message, span, lxssKey.get(), configuration, registration);
            }
        }
        else
        {
            // Create the directory to store the root filesystem.
            const auto rootFsPath = configuration.BasePath / LXSS_ROOTFS_DIRECTORY;
            wsl::windows::common::filesystem::CreateRootFs(rootFsPath.c_str(), configuration.Version);
            deleteFlags = LXSS_DELETE_DISTRO_FLAGS_ROOTFS;

            // Use bsdtar to extract the tar.gz file.
            auto mounts = _CreateSetupMounts(configuration);
            {
                auto elfContext = _RunElfBinary(
                    LXSS_BSDTAR_PATH " -C " LXSS_ROOTFS_MOUNT LXSS_BSDTAR_EXTRACT_ARGS,
                    configuration.BasePath.c_str(),
                    clientProcess.get(),
                    FileHandle,
                    nullptr,
                    ErrorHandle,
                    mounts.data(),
                    static_cast<ULONG>(mounts.size()));

                auto exitStatus = _GetElfExitStatus(elfContext);
                THROW_HR_IF(WSL_E_IMPORT_FAILED, exitStatus != 0);
            }

            // Invoke the init binary with the option to export the distribuiton information via stdout.
            {
                std::pair<wil::unique_handle, wil::unique_handle> input;
                THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&input.first, &input.second, nullptr, 0));

                std::pair<wil::unique_handle, wil::unique_handle> output;
                THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&output.first, &output.second, nullptr, 0));

                auto elfContext = _RunElfBinary(
                    LXSS_TOOLS_MOUNT "/init " LX_INIT_IMPORT_MESSAGE_ARG " " LXSS_ROOTFS_MOUNT,
                    configuration.BasePath.c_str(),
                    clientProcess.get(),
                    input.first.get(),
                    output.second.get(),
                    ErrorHandle,
                    mounts.data(),
                    static_cast<ULONG>(mounts.size()));

                // Close handles that were marshalled to WSL1.
                input.first.reset();
                output.second.reset();

                // Read the import result message from stdout.
                wil::unique_handle clientProcess = wsl::windows::common::wslutil::OpenCallingProcess(GENERIC_READ | SYNCHRONIZE);
                MESSAGE_HEADER header{};
                const auto headerSpan = gslhelpers::struct_as_writeable_bytes(header);
                auto bytesRead = wsl::windows::common::relay::InterruptableRead(
                    output.first.get(), gslhelpers::struct_as_writeable_bytes(header), {clientProcess.get()});

                THROW_HR_IF(WSL_E_IMPORT_FAILED, bytesRead != headerSpan.size() || header.MessageSize <= headerSpan.size() || header.MessageType != LxMiniInitMessageImportResult);

                std::vector<gsl::byte> buffer(header.MessageSize);
                const auto span = gsl::make_span(buffer);
                gsl::copy(headerSpan, span);

                auto offset = headerSpan.size();
                while (offset < span.size())
                {
                    bytesRead =
                        wsl::windows::common::relay::InterruptableRead(output.first.get(), span.subspan(offset), {clientProcess.get()});
                    if (bytesRead <= 0)
                    {
                        break;
                    }

                    offset += bytesRead;
                }

                THROW_HR_IF(WSL_E_IMPORT_FAILED, offset != buffer.size());

                // Close the stdin write handle to let init exit and process the import result message.
                input.second.reset();
                auto exitStatus = _GetElfExitStatus(elfContext);
                THROW_HR_IF(WSL_E_IMPORT_FAILED, exitStatus != 0);

                const auto message = gslhelpers::try_get_struct<LX_MINI_INIT_IMPORT_RESULT>(span);
                THROW_HR_IF(WSL_E_IMPORT_FAILED, !message);

                _ProcessImportResultMessage(*message, span, lxssKey.get(), configuration, registration);
            }
        }

        // Mark the distribution as installed and delete the scope exit variable
        // so the registration is persisted.
        {
            std::lock_guard lock(m_instanceLock);
            _SetDistributionInstalled(lxssKey.get(), registration.Id());
            cleanup.release();
        }

        _SendDistributionRegisteredEvent(configuration);

        _LaunchOOBEIfNeeded();

        *pDistroGuid = registration.Id();
        if (InstalledDistributionName != nullptr)
        {
            *InstalledDistributionName = wil::make_cotaskmem_string(configuration.Name.c_str()).release();
        }

        result = S_OK;
    }
    catch (...)
    {
        result = wil::ResultFromCaughtException();
    }

    return result;
}

HRESULT LxssUserSessionImpl::SetDefaultDistribution(_In_ LPCGUID DistroGuid)
try
{
    const wil::unique_hkey lxssKey = s_OpenLxssUserKey();

    // Ensure the distribution is in the installed state.
    std::lock_guard lock(m_instanceLock);

    const auto registration = DistributionRegistration::Open(lxssKey.get(), *DistroGuid);
    const DWORD state = registration.Read(Property::State);

    RETURN_HR_IF(WSL_E_DISTRO_NOT_FOUND, (state != LxssDistributionStateInstalled));

    // Set the distribution to the default.
    DistributionRegistration::SetDefault(lxssKey.get(), registration);

    return S_OK;
}
CATCH_RETURN()

HRESULT LxssUserSessionImpl::SetSparse(_In_ LPCGUID DistroGuid, _In_ BOOLEAN Sparse, _In_ BOOLEAN AllowUnsafe)
try
{
    auto runAsUser = wil::CoImpersonateClient();
    const wil::unique_hkey lxssKey = s_OpenLxssUserKey();
    std::lock_guard lock(m_instanceLock);

    const auto registration = DistributionRegistration::Open(lxssKey.get(), *DistroGuid);
    LXSS_DISTRO_CONFIGURATION configuration = s_GetDistributionConfiguration(registration);

    // Don't attempt on V1
    if (WI_IsFlagClear(configuration.Flags, LXSS_DISTRO_FLAGS_VM_MODE))
    {
        THROW_HR_WITH_USER_ERROR(WSL_E_VM_MODE_INVALID_STATE, wsl::shared::Localization::MessageSparseVhdWsl2Only());
    }

    // Allow disabling sparse mode but not enabling until the data corruption issue has been resolved.
    if (Sparse && !AllowUnsafe)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, wsl::shared::Localization::MessageSparseVhdDisabled());
    }

    // Don't attempt if running
    RETURN_HR_IF(WSL_E_DISTRO_NOT_STOPPED, m_runningInstances.contains(*DistroGuid));

    const wil::unique_hfile vhd{::CreateFileW(configuration.VhdFilePath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
    if (const DWORD err = GetLastError(); err == ERROR_SHARING_VIOLATION)
    {
        THROW_HR_WITH_USER_ERROR(HRESULT_FROM_WIN32(err), wsl::shared::Localization::MessageVhdInUse());
    }
    THROW_LAST_ERROR_IF(!vhd);

    FILE_SET_SPARSE_BUFFER buffer{
        .SetSparse = Sparse,
    };
    THROW_IF_WIN32_BOOL_FALSE(::DeviceIoControl(vhd.get(), FSCTL_SET_SPARSE, &buffer, sizeof(buffer), nullptr, 0, nullptr, nullptr));

    return S_OK;
}
CATCH_RETURN()

HRESULT LxssUserSessionImpl::ResizeDistribution(_In_ LPCGUID DistroGuid, _In_ HANDLE OutputHandle, _In_ ULONG64 NewSize)
try
{
    std::lock_guard lock(m_instanceLock);
    const wil::unique_hkey lxssKey = s_OpenLxssUserKey();
    const auto registration = DistributionRegistration::Open(lxssKey.get(), *DistroGuid);
    LXSS_DISTRO_CONFIGURATION configuration = s_GetDistributionConfiguration(registration);
    RETURN_HR_IF(WSL_E_WSL2_NEEDED, WI_IsFlagClear(configuration.Flags, LXSS_DISTRO_FLAGS_VM_MODE));

    const auto vhdFilePath = configuration.VhdFilePath;
    if (m_utilityVm && m_utilityVm->IsVhdAttached(vhdFilePath.c_str()))
    {
        THROW_HR_WITH_USER_ERROR(WSL_E_DISTRO_NOT_STOPPED, wsl::shared::Localization::MessageVhdInUse());
    }

    auto diskHandle = wsl::core::filesystem::OpenVhd(vhdFilePath.c_str(), VIRTUAL_DISK_ACCESS_GET_INFO | VIRTUAL_DISK_ACCESS_METAOPS);
    const auto diskSize = wsl::core::filesystem::GetDiskSize(diskHandle.get());

    const auto resizingLarger = NewSize > diskSize;
    if (resizingLarger)
    {
        wsl::core::filesystem::ResizeExistingVhd(diskHandle.get(), NewSize, RESIZE_VIRTUAL_DISK_FLAG_NONE);
    }

    diskHandle.reset();

    // Ensure VM exists and attach the VHD.
    _CreateVm();
    const auto userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
    const auto lun = m_utilityVm->AttachDisk(vhdFilePath.c_str(), WslCoreVm::DiskType::VHD, {}, true, userToken.get());

    // Resize the underlying filesystem.
    //
    // N.B. Passing zero as the size causes the resize to consume all available space on the block device.
    {
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { m_utilityVm->EjectVhd(vhdFilePath.c_str()); });
        m_utilityVm->ResizeDistribution(lun, OutputHandle, resizingLarger ? 0 : NewSize);
    }

    // If shrinking the VHD, resize the underlying VHD file. This is only supported for .vhdx files.
    //
    // N.B. RESIZE_VIRTUAL_DISK_FLAG_ALLOW_UNSAFE_VIRTUAL_SIZE is required because vhdmp can't validate that the minimum safe ext4 size.
    if (!resizingLarger &&
        wsl::shared::string::IsEqual(vhdFilePath.extension().c_str(), wsl::windows::common::wslutil::c_vhdxFileExtension, true))
    {
        const auto diskHandle =
            wsl::core::filesystem::OpenVhd(vhdFilePath.c_str(), VIRTUAL_DISK_ACCESS_GET_INFO | VIRTUAL_DISK_ACCESS_METAOPS);
        wsl::core::filesystem::ResizeExistingVhd(diskHandle.get(), NewSize, RESIZE_VIRTUAL_DISK_FLAG_ALLOW_UNSAFE_VIRTUAL_SIZE);
    }

    return S_OK;
}
CATCH_RETURN()

HRESULT LxssUserSessionImpl::SetVersion(_In_ LPCGUID DistroGuid, _In_ ULONG Version, _In_ HANDLE StderrHandle)
{
    RETURN_HR_IF(E_INVALIDARG, ((Version != LXSS_WSL_VERSION_1) && (Version != LXSS_WSL_VERSION_2)));

    DistributionRegistration registration;
    LXSS_DISTRO_CONFIGURATION configuration;
    wil::unique_hkey lxssKey = s_OpenLxssUserKey();
    try
    {
        // Ensure the distribution exists.
        std::lock_guard lock(m_instanceLock);
        registration = DistributionRegistration::Open(lxssKey.get(), *DistroGuid);
        configuration = s_GetDistributionConfiguration(registration);

        // The distro must be in the installed state.
        RETURN_HR_IF(E_ILLEGAL_STATE_CHANGE, (configuration.State != LxssDistributionStateInstalled));

        // Ensure distro is not already in the requested state.
        if (Version == LXSS_WSL_VERSION_1)
        {
            RETURN_HR_IF(WSL_E_VM_MODE_INVALID_STATE, WI_IsFlagClear(configuration.Flags, LXSS_DISTRO_FLAGS_VM_MODE));
        }
        else
        {
            // The legacy distribution does not support VM mode.
            RETURN_HR_IF(WSL_E_VM_MODE_NOT_SUPPORTED, (configuration.Version == LXSS_DISTRO_VERSION_LEGACY));

            RETURN_HR_IF(WSL_E_VM_MODE_INVALID_STATE, WI_IsFlagSet(configuration.Flags, LXSS_DISTRO_FLAGS_VM_MODE));
        }

        // Conversion is not possible if the lxcore driver is not present.
        RETURN_HR_IF(WSL_E_WSL1_NOT_SUPPORTED, !g_lxcoreInitialized);

        // Add the distribution to the list of converting distributions.
        _ConversionBegin(configuration.DistroId, LxssDistributionStateConverting);

        // Remove the distribution ID from m_updatedInitDistros so init is updated on the next launch (in the case of a conversion to WSL1).
        m_updatedInitDistros.erase(
            std::remove(m_updatedInitDistros.begin(), m_updatedInitDistros.end(), configuration.DistroId), m_updatedInitDistros.end());
    }
    CATCH_RETURN()

    // Set up a scope exit member to remove the distribution from the converting list.
    auto conversionComplete = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { _ConversionComplete(configuration.DistroId); });

    // Log telemetry to track how long enabling VM mode takes.
    WSL_LOG_TELEMETRY(
        "SetVersionBegin",
        PDT_ProductAndServicePerformance,
        TraceLoggingValue(configuration.Name.c_str(), "distroName"),
        TraceLoggingValue(Version, "version"));

    HRESULT result;
    auto setVersionComplete = wil::scope_exit([&] {
        WSL_LOG_TELEMETRY(
            "SetVersionEnd",
            PDT_ProductAndServicePerformance,
            TraceLoggingValue(configuration.Name.c_str(), "distroName"),
            TraceLoggingValue(Version, "version"),
            TraceLoggingValue(result, "result"));
    });

    try
    {
        ULONG deleteFlags = 0;
        wil::unique_handle userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            auto runAsUser = wil::impersonate_token(userToken.get());
            _DeleteDistribution(configuration, deleteFlags);
        });

        bool wroteLf = false;
        size_t lastIndex = -1;
        auto onTarOutput = [&StderrHandle, &wroteLf, &lastIndex](size_t Index, const gsl::span<gsl::byte>& Content) {
            WI_ASSERT(Index == 0 || Index == 1);
            auto it = Content.begin();

            while (it != Content.end())
            {
                if (wroteLf || lastIndex != Index)
                {
                    if (*it == static_cast<std::byte>('\n') && lastIndex != Index)
                    {
                        it++;
                        continue;
                    }

                    // Add an extra newline if the input index changed to avoid mixing lines.
                    if (lastIndex != Index && !wroteLf)
                    {
                        THROW_IF_WIN32_BOOL_FALSE(WriteFile(StderrHandle, "\n", 1, nullptr, nullptr));
                    }

                    THROW_IF_WIN32_BOOL_FALSE(WriteFile(StderrHandle, Index == 0 ? "wsl1: " : "wsl2: ", 6, nullptr, nullptr));
                    wroteLf = false;
                    lastIndex = Index;
                }

                auto lf = std::find(it, Content.end(), static_cast<std::byte>('\n'));
                if (lf != Content.end())
                {
                    lf++;
                    wroteLf = true;
                }

                THROW_IF_WIN32_BOOL_FALSE(WriteFile(StderrHandle, &*it, static_cast<DWORD>(lf - it), nullptr, nullptr));

                it = lf;
            }
        };

        wil::unique_handle clientProcess = wsl::windows::common::wslutil::OpenCallingProcess(GENERIC_READ | SYNCHRONIZE);
        std::string commandLine{LXSS_BSDTAR_PATH};
        ULONG newFlags = configuration.Flags;
        if (Version == LXSS_WSL_VERSION_1)
        {
            auto policiesKey = wsl::windows::policies::OpenPoliciesKey();
            if (!wsl::windows::policies::IsFeatureAllowed(policiesKey.get(), wsl::windows::policies::c_allowWSL1))
            {
                THROW_HR_WITH_USER_ERROR(WSL_E_WSL1_DISABLED, wsl::shared::Localization::MessageWSL1Disabled());
            }

            auto rootfsPath = configuration.BasePath / LXSS_ROOTFS_DIRECTORY;

            // Ensure the target directory is empty and create the root filesystem.
            {
                std::lock_guard lock(m_instanceLock);
                {
                    auto runAsUser = wil::impersonate_token(userToken.get());
                    _DeleteDistributionLockHeld(configuration, LXSS_DELETE_DISTRO_FLAGS_ROOTFS);
                }

                wsl::windows::common::filesystem::CreateRootFs(rootfsPath.c_str(), configuration.Version);
                deleteFlags = LXSS_DELETE_DISTRO_FLAGS_ROOTFS;
            }

            // Create a utility VM to create the tar file and output it via a
            // socket.
            auto vmContext = _RunUtilityVmSetup(configuration, LxMiniInitMessageExport, 0, true);

            auto wsl1Pipe = wsl::windows::common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);

            wsl::windows::common::relay::ScopedMultiRelay stdErrRelay(
                std::vector<HANDLE>{wsl1Pipe.first.get(), reinterpret_cast<HANDLE*>(vmContext.errorSocket.get())}, onTarOutput);

            // Add mounts for the rootfs and tools.
            auto mounts = _CreateSetupMounts(configuration);

            if (m_utilityVm->GetConfig().SetVersionDebug)
            {
                commandLine += " -v";
            }

            // Run the bsdtar elf binary expand the the tar file using the socket as stdin.
            commandLine += " -C " LXSS_ROOTFS_MOUNT LXSS_BSDTAR_EXTRACT_ARGS;
            auto elfContext = _RunElfBinary(
                commandLine.c_str(),
                configuration.BasePath.c_str(),
                clientProcess.get(),
                reinterpret_cast<HANDLE>(vmContext.tarSocket.get()),
                nullptr,
                wsl1Pipe.second.get(),
                mounts.data(),
                static_cast<ULONG>(mounts.size()));

            wsl1Pipe.second.reset();

            // Wait for the utility VM to finish creating the tar and ensure that
            // the export was successful.
            LONG exitStatus = 1;
            vmContext.instance->GetInitPort()->Receive(&exitStatus, sizeof(exitStatus), clientProcess.get());
            THROW_HR_IF(WSL_E_EXPORT_FAILED, (exitStatus != 0));

            // Wait for the elf binary to finish expanding the tar and ensure
            // that it was successful.
            exitStatus = _GetElfExitStatus(elfContext);
            THROW_HR_IF(WSL_E_IMPORT_FAILED, exitStatus != 0);

            // Import from the vhd was successful.
            deleteFlags = LXSS_DELETE_DISTRO_FLAGS_VHD | LXSS_DELETE_DISTRO_FLAGS_WSLG_SHORTCUTS;
            WI_ClearFlag(newFlags, LXSS_DISTRO_FLAGS_VM_MODE);
        }
        else
        {
            {
                std::lock_guard lock(m_instanceLock);
                _CreateVm();
            }

            // Create a vhd to store the root filesystem.
            {
                auto runAsUser = wil::impersonate_token(userToken.get());
                wsl::core::filesystem::CreateVhd(
                    configuration.VhdFilePath.c_str(),
                    m_utilityVm->GetConfig().VhdSizeBytes,
                    GetUserSid(),
                    m_utilityVm->GetConfig().EnableSparseVhd,
                    false);

                deleteFlags = LXSS_DELETE_DISTRO_FLAGS_VHD;
            }

            // Create a process in the utility VM to expand the tar file from a socket.
            auto vmContext = _RunUtilityVmSetup(configuration, LxMiniInitMessageImport, 0, true);

            auto wsl1Pipe = wsl::windows::common::wslutil::OpenAnonymousPipe(LX_RELAY_BUFFER_SIZE, true, true);

            wsl::windows::common::relay::ScopedMultiRelay stdErrRelay(
                std::vector<HANDLE>{wsl1Pipe.first.get(), reinterpret_cast<HANDLE*>(vmContext.errorSocket.get())}, onTarOutput);

            // Add mounts for the rootfs and tools.
            auto mounts = _CreateSetupMounts(configuration);

            if (m_utilityVm->GetConfig().SetVersionDebug)
            {
                commandLine += " -v";
            }

            // Run the bsdtar elf binary to create the tar file using the socket as stdout.
            commandLine += " -C " LXSS_ROOTFS_MOUNT LXSS_BSDTAR_CREATE_ARGS;
            auto elfContext = _RunElfBinary(
                commandLine.c_str(),
                configuration.BasePath.c_str(),
                clientProcess.get(),
                nullptr,
                reinterpret_cast<HANDLE>(vmContext.tarSocket.get()),
                wsl1Pipe.second.get(),
                mounts.data(),
                static_cast<ULONG>(mounts.size()));

            wsl1Pipe.second.reset();

            LONG exitStatus = _GetElfExitStatus(elfContext);
            THROW_HR_IF(WSL_E_IMPORT_FAILED, exitStatus != 0);

            // Close the socket now that all data has been written.
            vmContext.tarSocket.reset();

            // Wait for the utility VM to finish expanding the tar and ensure that
            // the export was successful.
            auto* channel = dynamic_cast<WslCoreInstance::WslCorePort*>(vmContext.instance->GetInitPort().get());

            gsl::span<gsl::byte> span;
            const auto& message = channel->GetChannel().ReceiveMessage<LX_MINI_INIT_IMPORT_RESULT>(&span);
            THROW_HR_IF(E_FAIL, (message.Result != 0));

            if (message.FlavorIndex > 0)
            {
                configuration.Flavor = wsl::shared::string::MultiByteToWide(wsl::shared::string::FromSpan(span, message.FlavorIndex));
                registration.Write(Property::Flavor, configuration.Flavor.c_str());
            }

            if (message.VersionIndex > 0)
            {
                configuration.OsVersion = wsl::shared::string::MultiByteToWide(wsl::shared::string::FromSpan(span, message.VersionIndex));
                registration.Write(Property::OsVersion, configuration.OsVersion.c_str());
            }

            // Operation was successful.
            deleteFlags = LXSS_DELETE_DISTRO_FLAGS_ROOTFS;
            WI_SetFlag(newFlags, LXSS_DISTRO_FLAGS_VM_MODE);
        }

        // Record the new distribution state.
        registration.Write(Property::Flags, newFlags);

        result = S_OK;
    }
    catch (...)
    {
        result = wil::ResultFromCaughtException();
    }

    return result;
}

HRESULT LxssUserSessionImpl::Shutdown(_In_ bool PreventNewInstances, bool ForceTerminate)
{
    try
    {
        // If the user asks for a forced termination, kill the VM
        if (ForceTerminate)
        {
            auto vmId = m_vmId.load();
            if (!IsEqualGUID(vmId, GUID_NULL))
            {
                auto vmIdStr = wsl::shared::string::GuidToString<wchar_t>(vmId, wsl::shared::string::GuidToStringFlags::Uppercase);

                auto result = wil::ResultFromException([&]() {
                    auto computeSystem = wsl::windows::common::hcs::OpenComputeSystem(vmIdStr.c_str(), GENERIC_ALL);
                    wsl::windows::common::hcs::TerminateComputeSystem(computeSystem.get());
                });

                WSL_LOG("ForceTerminateVm", TraceLoggingValue(result, "Result"));
            }
        }

        {
            // Stop each instance with the lock held.
            std::lock_guard lock(m_instanceLock);
            while (!m_runningInstances.empty())
            {
                _TerminateInstanceInternal(&m_runningInstances.begin()->first, false);
            }

            // Terminate the utility VM.
            _VmTerminate();

            // Reset the proxy state.
            // We don't clear it in _VMTerminate because we want to cache results if possible.
            m_httpProxyStateTracker.reset();

            // Clear any attached disk state.
            // This is needed because wsl --shutdown might be called after the vm
            // has timed out (and so the disks states would have been written in the registry)
            const auto key = wsl::windows::common::registry::OpenOrCreateLxssDiskMountsKey(&m_userSid.Sid);
            wsl::windows::common::registry::ClearSubkeys(key.get());

            WI_ASSERT(!PreventNewInstances || !m_disableNewInstanceCreation);

            // This is used when the session is being deleted.
            // This is in place to prevent a CreateInstace() call from succeeding
            // after the session is shut down since this would mean that the destructor,
            // which could run on that thread (if the session already dropped its LxssUserSessionImpl reference)
            // would have to do all the cleanup work.
            m_disableNewInstanceCreation = PreventNewInstances;
        }

        auto lock = m_terminatedInstanceLock.lock_exclusive();
        m_terminatedInstances.clear();
    }
    CATCH_LOG()

    return S_OK;
}

void LxssUserSessionImpl::TelemetryWorker(_In_ wil::unique_socket&& socket, _In_ bool drvFsNotifications) const
try
{
    wsl::windows::common::wslutil::SetThreadDescription(L"Telemetry");

    wsl::shared::SocketChannel channel(std::move(socket), "Telemetry", m_vmTerminating.get());

    // Aggregate information about what is running inside the VM. This is logged
    // periodically because logging each event individually would be too noisy.
    for (;;)
    {
        auto [Message, Span] = channel.ReceiveMessageOrClosed<LX_MINI_INIT_TELEMETRY_MESSAGE>();
        if (Message == nullptr)
        {
            break;
        }

        std::map<std::string, size_t> events{};

        std::string content = wsl::shared::string::FromSpan(Span, offsetof(LX_MINI_INIT_TELEMETRY_MESSAGE, Buffer));
        auto values = wsl::shared::string::Split<char>(content, '/');

        THROW_HR_IF(E_UNEXPECTED, values.size() % 2 != 0);

        // Periodically log an event to track active WSL usage. This event must be marked as
        // 'MICROSOFT_KEYWORD_CRITICAL_DATA' and not MICROSOFT_KEYWORD_MEASURES.
        //
        // N.B. The count and imageName values are unused but required because they were present in the approved critical event.
        WSL_LOG(
            "ExecCritical",
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_CRITICAL_DATA),
            TraceLoggingValue(0, "count"),
            TraceLoggingValue("", "imageName"),
            TraceLoggingLevel(WINEVENT_LEVEL_INFO));

        for (size_t i = 0; i < values.size(); i += 2)
        {
            // Log an aggregated account of the binary names run in WSL and their counts, used to determine popular use cases and prioritize support for issues
            WSL_LOG_TELEMETRY(
                "Exec",
                PDT_ProductAndServiceUsage,
                TraceLoggingValue(std::stoull(values[i + 1]), "count"),
                TraceLoggingValue(values[i].c_str(), "imageName"),
                TraceLoggingLevel(WINEVENT_LEVEL_INFO));
        }

        if (drvFsNotifications && Message->ShowDrvFsNotification && !values.empty())
        {
            // If a drvfs notification is requested, the first entry is the executable that triggered it.
            LOG_IF_FAILED(wsl::windows::common::notifications::DisplayFilesystemNotification(values[0].c_str()));
            drvFsNotifications = false;
        }
    }
}
CATCH_LOG()

_Requires_lock_not_held_(m_instanceLock)
void LxssUserSessionImpl::TerminateByClientId(_In_ ULONG ClientId)
{
    if (ClientId == LXSS_CLIENT_ID_INVALID)
    {
        return;
    }

    std::lock_guard lock(m_instanceLock);
    TerminateByClientIdLockHeld(ClientId);
}

_Requires_lock_held_(m_instanceLock)
void LxssUserSessionImpl::TerminateByClientIdLockHeld(_In_ ULONG ClientId)
{
    // Terminate any instances with a matching client ID.
    std::vector<GUID> instances;
    std::for_each(m_runningInstances.begin(), m_runningInstances.end(), [&](auto& pair) {
        auto id = pair.second->GetClientId();
        if ((id == ClientId) || ((ClientId == LXSS_CLIENT_ID_WILDCARD) && (id != LXSS_CLIENT_ID_INVALID)))
        {
            instances.push_back(pair.first);
        }
    });

    std::for_each(instances.begin(), instances.end(), [&](auto& guid) { _TerminateInstanceInternal(&guid, false); });

    // If the wildcard client ID was specified, the utility VM unexpectedly exited.
    if (ClientId == LXSS_CLIENT_ID_WILDCARD)
    {
        _VmTerminate();
    }
}

HRESULT LxssUserSessionImpl::TerminateDistribution(_In_opt_ LPCGUID DistroGuid)
try
{
    const wil::unique_hkey lxssKey = s_OpenLxssUserKey();
    GUID defaultDistro;
    {
        std::lock_guard lock(m_instanceLock);

        // If no distribution GUID was supplied, use the default.
        if (ARGUMENT_PRESENT(DistroGuid) == FALSE)
        {
            defaultDistro = _GetDefaultDistro(lxssKey.get());
            DistroGuid = &defaultDistro;
        }

        _TerminateInstanceInternal(DistroGuid);
    }

    return S_OK;
}
CATCH_RETURN()

HRESULT LxssUserSessionImpl::UnregisterDistribution(_In_ LPCGUID DistroGuid)
{
    ExecutionContext context(Context::UnregisterDistro);

    // Set up a scope exit member to log unregistration status.
    DistributionRegistration registration;
    LXSS_DISTRO_CONFIGURATION configuration{};
    HRESULT result = E_FAIL;
    auto unregisterExit = wil::scope_exit([&] {
        // Only log the end event if a distro was found.
        if (configuration.Name.size() > 0)
        {
            WSL_LOG(
                "UnregisterDistributionEnd",
                TraceLoggingValue(configuration.Name.c_str(), "name"),
                TraceLoggingHexUInt32(result, "result"));
        }
    });

    try
    {
        wil::unique_hkey lxssKey = s_OpenLxssUserKey();

        // Set up a scope exit lambda to delete the distribution registry key
        // when the function exits.
        auto removedDistroString = wsl::shared::string::GuidToString<wchar_t>(*DistroGuid);
        bool removeDistro = false;
        auto deleteDistroKey = wil::scope_exit([&] {
            if (removeDistro)
            {
                wsl::windows::common::registry::DeleteKey(lxssKey.get(), removedDistroString.c_str());
            }
        });

        {
            std::lock_guard lock(m_instanceLock);

            // Get the configuration information about the distribution.
            registration = DistributionRegistration::Open(lxssKey.get(), *DistroGuid);
            configuration = s_GetDistributionConfiguration(registration);

            // Log telemetry about the distribution being removed.
            WSL_LOG_TELEMETRY(
                "UnregisterDistributionBegin", PDT_ProductAndServiceUsage, TraceLoggingValue(configuration.Name.c_str(), "name"));

            // Ensure that a filesystem export is not in progress.
            _EnsureNotLocked(DistroGuid);

            // After this point the distribution registry key should be deleted.
            removeDistro = true;

            // Terminate the distribution and mark it as uninstalling.
            _TerminateInstanceInternal(DistroGuid);
            registration.Write(Property::State, LxssDistributionStateUninstalling);

            // If the default distribution has been unregistered, search for another
            // distribution to set as the new default.

            auto defaultDistribution = DistributionRegistration::OpenDefault(lxssKey.get());
            if (defaultDistribution.has_value() && IsEqualGUID(defaultDistribution->Id(), registration.Id()))
            {
                // Remove the old default.
                DistributionRegistration::DeleteDefault(lxssKey.get());

                // If there are any other registered distributions, set the first
                // one found to the new default.
                auto distributions = _EnumerateDistributions(lxssKey.get());
                if (distributions.size() > 0)
                {
                    DistributionRegistration::SetDefault(lxssKey.get(), distributions[0]);
                }
            }

            {
                auto runAsUser = wil::CoImpersonateClient();
                _DeleteDistributionLockHeld(configuration);
            }

            WslOfflineDistributionInformation distributionInfo;
            distributionInfo.Id = configuration.DistroId;
            distributionInfo.Name = configuration.Name.c_str();
            distributionInfo.PackageFamilyName = configuration.PackageFamilyName.c_str();
            distributionInfo.Flavor = configuration.Flavor.empty() ? nullptr : configuration.Flavor.c_str();
            distributionInfo.Version = configuration.OsVersion.empty() ? nullptr : configuration.OsVersion.c_str();

            m_pluginManager.OnDistributionUnregistered(&m_session, &distributionInfo);
        }

        result = S_OK;
    }
    catch (...)
    {
        result = wil::ResultFromCaughtException();
    }

    return result;
}

_Requires_lock_held_(m_instanceLock)
void LxssUserSessionImpl::_ConversionBegin(_In_ GUID DistroGuid, _In_ LxssDistributionState State)
{
    _EnsureNotLocked(&DistroGuid);
    _TerminateInstanceInternal(&DistroGuid);
    m_lockedDistributions.emplace_back(DistroGuid, State);
}

_Requires_lock_not_held_(m_instanceLock)
void LxssUserSessionImpl::_ConversionComplete(_In_ GUID DistroGuid)
{
    std::lock_guard lock(m_instanceLock);
    std::erase_if(m_lockedDistributions, [&](const auto& pair) { return (IsEqualGUID(pair.first, DistroGuid)); });

    _VmCheckIdle();
}

_Requires_exclusive_lock_held_(m_instanceLock)
void LxssUserSessionImpl::_CreateLegacyRegistration(_In_ HKEY LxssKey, _In_ HANDLE UserToken)
{
    // Delete any existing legacy registration.
    const auto distroGuidString = wsl::shared::string::GuidToString<wchar_t>(LXSS_LEGACY_DISTRO_GUID);
    wsl::windows::common::registry::DeleteKey(LxssKey, distroGuidString.c_str());

    // Migrate legacy default user configuration.
    const ULONG defaultUid = wsl::windows::common::registry::ReadDword(LxssKey, nullptr, WSL_DISTRO_CONFIG_DEFAULT_UID, LX_UID_ROOT);
    DWORD configFlags = LXSS_DISTRO_FLAGS_DEFAULT;
    DWORD enabled = wsl::windows::common::registry::ReadDword(LxssKey, nullptr, LXSS_LEGACY_APPEND_NT_PATH, 1);
    WI_ClearFlagIf(configFlags, LXSS_DISTRO_FLAGS_APPEND_NT_PATH, (enabled == 0));
    enabled = wsl::windows::common::registry::ReadDword(LxssKey, nullptr, LXSS_LEGACY_INTEROP_ENABLED, 1);
    WI_ClearFlagIf(configFlags, LXSS_DISTRO_FLAGS_ENABLE_INTEROP, (enabled == 0));

    // Create a new registration for the legacy distro.
    const auto basePath = wsl::windows::common::filesystem::GetLegacyBasePath(UserToken);

    DistributionRegistration::Create(
        LxssKey, LXSS_LEGACY_DISTRO_GUID, LXSS_LEGACY_INSTALL_NAME, LXSS_DISTRO_VERSION_LEGACY, basePath.c_str(), configFlags, defaultUid, nullptr, LXSS_VM_MODE_VHD_NAME, false);

    _SetDistributionInstalled(LxssKey, LXSS_LEGACY_DISTRO_GUID);
}

std::vector<wsl::windows::common::filesystem::unique_lxss_addmount> LxssUserSessionImpl::_CreateSetupMounts(_In_ const LXSS_DISTRO_CONFIGURATION& Configuration)
{
    // Add a rootfs mount.
    auto runAsUser = wil::CoImpersonateClient();
    const auto rootFsPath = Configuration.BasePath / LXSS_ROOTFS_DIRECTORY;
    std::vector<wsl::windows::common::filesystem::unique_lxss_addmount> mounts;
    mounts.emplace_back(wsl::windows::common::filesystem::CreateMount(
        rootFsPath.c_str(), LXSS_ROOTFS_DIRECTORY, LXSS_ROOTFS_MOUNT, LXSS_DISTRO_USES_WSL_FS(Configuration.Version) ? LXSS_FS_TYPE_WSLFS : LXSS_FS_TYPE_LXFS, 0755));

    // Add a read only sharefs mount to the inbox tools directory which contains the bsdtar binary.
    std::wstring systemDirectory;
    THROW_IF_FAILED(wil::GetSystemDirectoryW(systemDirectory));

    // Add a read only sharefs mount to the packaged tools directory which contains the init binary.
    const auto initPath = wsl::windows::common::wslutil::GetBasePath() / L"tools";
    mounts.emplace_back(wsl::windows::common::filesystem::CreateMount(
        initPath.c_str(), initPath.c_str(), LXSS_TOOLS_MOUNT, LXSS_FS_TYPE_SHAREFS, 0755, false));

    return mounts;
}

_Requires_lock_not_held_(m_instanceLock)
std::shared_ptr<LxssRunningInstance> LxssUserSessionImpl::_CreateInstance(_In_opt_ LPCGUID DistroGuid, _In_ ULONG Flags)
{
    ExecutionContext context(Context::CreateInstance);

    // Validate flags.
    THROW_HR_IF(E_INVALIDARG, (WI_IsAnyFlagSet(Flags, ~LXSS_CREATE_INSTANCE_FLAGS_ALL)));

    // Clear the list of terminated instances before acquiring the instance
    // list lock.
    {
        auto lock = m_terminatedInstanceLock.lock_exclusive();
        m_terminatedInstances.clear();
    }

    wil::unique_hkey lxssKey = s_OpenLxssUserKey();
    DistributionRegistration registration;
    wil::unique_handle userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);

    std::shared_ptr<LxssRunningInstance> instance;
    {
        std::lock_guard lock(m_instanceLock);

        // m_disableNewInstanceCreation is set when the session is being deleted.
        // In that code path, don't create a new session.
        THROW_HR_IF(RPC_E_DISCONNECTED, m_disableNewInstanceCreation);

        registration = DistributionRegistration::OpenOrDefault(lxssKey.get(), DistroGuid);

        // Check if an instance is already running for this distribution, if
        // not create one.
        instance = _RunningInstance(&registration.Id());
        if (!instance)
        {
            THROW_HR_IF(E_NOT_SET, WI_IsFlagSet(Flags, LXSS_CREATE_INSTANCE_FLAGS_OPEN_EXISTING));

            // Query information about the distribution.
            auto configuration = s_GetDistributionConfiguration(registration);
            auto defaultUid = registration.Read(Property::DefaultUid);

            THROW_HR_IF(E_ILLEGAL_STATE_CHANGE, (configuration.State != LxssDistributionStateInstalled));

            // Determine the distribution version.
            ULONG version = WI_IsFlagSet(configuration.Flags, LXSS_DISTRO_FLAGS_VM_MODE) ? LXSS_WSL_VERSION_2 : LXSS_WSL_VERSION_1;

            // Create a GUID for the instance.
            GUID instanceId;
            THROW_IF_FAILED(CoCreateGuid(&instanceId));

            // Log telemetry to determine how long instance creation takes.
            WSL_LOG_TELEMETRY(
                "CreateInstanceBegin",
                PDT_ProductAndServicePerformance,
                TraceLoggingValue(configuration.Name.c_str(), "distroName"),
                TraceLoggingValue(version, "version"),
                TraceLoggingValue(instanceId, "instanceId"));

            HRESULT result = E_UNEXPECTED;

            auto createEnd = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
                WSL_LOG_TELEMETRY(
                    "CreateInstanceEnd",
                    PDT_ProductAndServicePerformance,
                    TraceLoggingValue(configuration.Name.c_str(), "distroName"),
                    TraceLoggingValue(version, "version"),
                    TraceLoggingValue(instanceId, "instanceId"),
                    TraceLoggingValue(SUCCEEDED(result), "success"),
                    TraceLoggingValue(result, "error"));
            });

            try
            {
                auto clientKey = m_lifetimeManager.GetRegistrationId();
                if (version == LXSS_WSL_VERSION_1)
                {
                    auto key = wsl::windows::policies::OpenPoliciesKey();
                    if (!wsl::windows::policies::IsFeatureAllowed(key.get(), wsl::windows::policies::c_allowWSL1))
                    {
                        THROW_HR_WITH_USER_ERROR(
                            WSL_E_WSL1_DISABLED,
                            wsl::shared::Localization::MessageWSL1Disabled() + L"\n" +
                                wsl::shared::Localization::MessageUpgradeToWSL2(configuration.Name));
                    }

                    instance = std::make_shared<LxssInstance>(
                        instanceId,
                        configuration,
                        defaultUid,
                        clientKey,
                        std::bind(s_TerminateInstance, this, registration.Id(), false),
                        std::bind(s_UpdateInit, this, configuration),
                        Flags,
                        _GetResultantConfig(userToken.get()).InstanceIdleTimeout);
                }
                else
                {
                    // Ensure the VM has been created.
                    _CreateVm();
                    instance = m_utilityVm->CreateInstance(
                        instanceId, configuration, LxMiniInitMessageLaunchInit, m_utilityVm->GetConfig().KernelBootTimeout, defaultUid, clientKey);
                }

                // Log telemetry to determine how long initialization takes.
                WSL_LOG(
                    "InitializeInstanceBegin",
                    TraceLoggingValue(configuration.Name.c_str(), "distroName"),
                    TraceLoggingValue(version, "version"),
                    TraceLoggingValue(instanceId, "instanceId"));

                auto initializeEnd = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
                    WSL_LOG(
                        "InitializeInstanceEnd",
                        TraceLoggingKeyword(MICROSOFT_KEYWORD_CRITICAL_DATA),
                        TraceLoggingValue(configuration.Name.c_str(), "distroName"),
                        TraceLoggingValue(version, "version"),
                        TraceLoggingValue(instanceId, "instanceId"));
                });

                // Initialize the instance and add it to the list of running instances.
                instance->Initialize();

                const auto* distributionInfo = instance->DistributionInformation();
                if (distributionInfo->Flavor != nullptr && distributionInfo->Flavor != configuration.Flavor)
                {
                    WSL_LOG(
                        "DistributionFlavorChange",
                        TraceLoggingValue(distributionInfo->Flavor, "NewFlavor"),
                        TraceLoggingValue(configuration.Flavor.c_str(), "OldFlavor"),
                        TraceLoggingValue(configuration.Name.c_str(), "distroName"),
                        TraceLoggingValue(instanceId, "instanceId"));

                    registration.Write(Property::Flavor, distributionInfo->Flavor);
                }

                if (distributionInfo->Version != nullptr && distributionInfo->Version != configuration.OsVersion)
                {
                    WSL_LOG(
                        "DistributionVersionChange",
                        TraceLoggingValue(distributionInfo->Version, "NewVersion"),
                        TraceLoggingValue(configuration.OsVersion.c_str(), "OldVersion"),
                        TraceLoggingValue(configuration.Name.c_str(), "distroName"),
                        TraceLoggingValue(instanceId, "instanceId"));

                    registration.Write(Property::OsVersion, distributionInfo->Version);
                }

                // This needs to be done before plugins are notifed because they might try to run a command inside the distribution.
                m_runningInstances[registration.Id()] = instance;

                if (version == LXSS_WSL_VERSION_2)
                {
                    auto cleanupOnFailure =
                        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { m_runningInstances.erase(registration.Id()); });
                    m_pluginManager.OnDistributionStarted(&m_session, instance->DistributionInformation());
                    cleanupOnFailure.release();
                }

                result = S_OK;
            }
            catch (...)
            {
                result = wil::ResultFromCaughtException();
                throw;
            }
        }
    }

    // Register the Plan 9 Redirector connection targets for the calling user if necessary.
    // N.B. Normally this is only necessary when creating the instance, and every subsequent time
    //      it's skipped because the user is already registered. However, in rare cases the
    //      instance is created by the same user but under a different context, with a different
    //      authentication ID, than the user's interactive session. For example, if the instance
    //      was created by a scheduled task. For this reason, ensure that the calling user is
    //      registered even for already running instances.
    instance->RegisterPlan9ConnectionTarget(userToken.get());

    // Determine the idle timeout for the instance. A value of less than zero indicates that the instance
    // should never be idle-terminated.
    if (instance->GetIdleTimeout() >= 0)
    {
        // Register a client termination callback with the lifetime manager. If the
        // ignore client callback flag is specifed and there are no other clients,
        // the timer is immediately queued.
        wil::unique_handle currentProcess{};
        if (WI_IsFlagClear(Flags, LXSS_CREATE_INSTANCE_FLAGS_IGNORE_CLIENT))
        {
            currentProcess = wsl::windows::common::wslutil::OpenCallingProcess(GENERIC_READ | SYNCHRONIZE);
        }

        m_lifetimeManager.RegisterCallback(
            instance->GetLifetimeManagerId(),
            std::bind(s_TerminateInstance, this, registration.Id(), true),
            currentProcess.get(),
            instance->GetIdleTimeout());
    }

    // If the system distro flag was specified, return the system distro for the instance.
    //
    // N.B. The system distro is only supported for WSL2.
    if (WI_IsFlagSet(Flags, LXSS_CREATE_INSTANCE_FLAGS_USE_SYSTEM_DISTRO))
    {
        auto wslCoreInstance = std::dynamic_pointer_cast<WslCoreInstance>(instance);
        THROW_HR_IF(WSL_E_WSL2_NEEDED, !wslCoreInstance);

        instance = wslCoreInstance->GetSystemDistro();
        THROW_HR_IF(WSL_E_GUI_APPLICATIONS_DISABLED, !instance);
    }

    return instance;
}

// N.B. This methods expects the caller to impersonate the user.
void LxssUserSessionImpl::_CreateDistributionShortcut(_In_ LPCWSTR DistributionName, LPCWSTR ShortcutIcon, LPCWSTR ExecutablePath, DistributionRegistration& registration)
try
{
    const auto shellLink = wil::CoCreateInstance<IShellLink>(CLSID_ShellLink);

    auto shortcutPath = wsl::windows::common::filesystem::GetKnownFolderPath(FOLDERID_StartMenu, KF_FLAG_CREATE);
    shortcutPath /= DistributionName + std::wstring(L".lnk");

    THROW_IF_FAILED(shellLink->SetPath(ExecutablePath));

    // Construct the command line to set the working directory to the user's home directory.
    const auto commandLine = std::format(
        L"{} {} {} {}", WSL_DISTRIBUTION_ID_ARG, wsl::shared::string::GuidToString<wchar_t>(registration.Id()), WSL_CHANGE_DIRECTORY_ARG, WSL_CWD_HOME);

    THROW_IF_FAILED(shellLink->SetArguments(commandLine.c_str()));
    THROW_IF_FAILED(shellLink->SetIconLocation(ShortcutIcon, 0));

    Microsoft::WRL::ComPtr<IPersistFile> storage;
    THROW_IF_FAILED(shellLink->QueryInterface(IID_IPersistFile, &storage));
    THROW_IF_FAILED(storage->Save(shortcutPath.c_str(), true));

    registration.Write(Property::ShortcutPath, shortcutPath.c_str());
}
CATCH_LOG();

// N.B. This methods expects the caller to impersonate the user.
void LxssUserSessionImpl::_CreateTerminalProfile(
    _In_ const std::string_view& Template,
    _In_ _In_ const std::filesystem::path& IconPath,
    _In_ const LXSS_DISTRO_CONFIGURATION& Configuration,
    wsl::windows::service::DistributionRegistration& Registration)
try
{
    using namespace wsl::windows::common::string;
    using namespace wsl::windows::common::wslutil;
    using wsl::shared::string::WideToMultiByte;

    nlohmann::basic_json json;
    nlohmann::json::iterator profiles;

    try
    {
        json = nlohmann::json::parse(Template, nullptr, true, false);
        THROW_HR_IF(E_UNEXPECTED, !json.is_object());

        profiles = json.find("profiles");
        THROW_HR_IF(E_UNEXPECTED, profiles == json.end() || !profiles->is_array());
    }
    catch (const nlohmann::json::parse_error& e)
    {
        EMIT_USER_WARNING(wsl::shared::Localization::MessageFailedToParseTerminalProfile(e.what()));
        return;
    }
    catch (...)
    {
        auto error = WideToMultiByte(wsl::windows::common::wslutil::ErrorCodeToString(wil::ResultFromCaughtException()));
        EMIT_USER_WARNING(wsl::shared::Localization::MessageFailedToParseTerminalProfile(error));
        return;
    }

    auto distributionIdString = wsl::shared::string::GuidToString<wchar_t>(Registration.Id());
    auto distributionProfileId =
        wsl::shared::string::GuidToString<wchar_t>(CreateV5Uuid(WslTerminalNamespace, std::as_bytes(std::span{distributionIdString})));

    auto hideGeneratedProfileGuid = WideToMultiByte(wsl::shared::string::GuidToString<wchar_t>(
        CreateV5Uuid(GeneratedProfilesTerminalNamespace, std::as_bytes(std::span{Configuration.Name}))));

    bool foundHideProfile = false;

    for (auto& e : *profiles)
    {
        auto updates = e.find("updates");
        if (updates != e.end() && (*updates) == hideGeneratedProfileGuid)
        {
            foundHideProfile = true;
            continue;
        }

        std::wstring systemDirectory;
        THROW_IF_FAILED(wil::GetSystemDirectory(systemDirectory));

        e["commandline"] = WideToMultiByte(std::format(
            L"{}\\{} {} {} {} {}", systemDirectory, WSL_BINARY_NAME, WSL_DISTRIBUTION_ID_ARG, distributionIdString, WSL_CHANGE_DIRECTORY_ARG, WSL_CWD_HOME));

        e["name"] = WideToMultiByte(Configuration.Name);
        e["guid"] = WideToMultiByte(distributionProfileId);
        e["icon"] = IconPath.string();

        // See https://github.com/microsoft/terminal/pull/18195. Supported in terminal >= 1.23
        e["pathTranslationStyle"] = "wsl";

        if (!Configuration.Flavor.empty())
        {
            e["wsl.distribution-type"] = WideToMultiByte(Configuration.Flavor);
        }

        if (!Configuration.OsVersion.empty())
        {
            e["wsl.distribution-version"] = WideToMultiByte(Configuration.OsVersion);
        }
    }

    // Add an entry to hide the autogenerated terminal profile, if not provided by the distribution.
    if (!foundHideProfile)
    {
        nlohmann::json hideProfile{{"updates", hideGeneratedProfileGuid}, {"hidden", true}};

        profiles->insert(profiles->begin(), hideProfile);
    }

    auto targetFolder = wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"Microsoft" / L"Windows Terminal" /
                        L"Fragments" / L"Microsoft.WSL";

    wil::CreateDirectoryDeep(targetFolder.c_str());

    auto tempFilePath = wsl::windows::common::filesystem::GetTempFilename();
    auto targetPath = targetFolder / (distributionProfileId + L".json");

    // Unfortunately creating & writing the file isn't atomic.
    // Creating the file somewhere else and then moving it to 'targetPath' isn't an option either, because MoveFile
    // will set its ownership to the Administrators group, which breaks terminal.
    wil::unique_handle file{CreateFile(targetPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr)};
    THROW_LAST_ERROR_IF(!file);

    auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        file.reset();
        DeleteFile(targetPath.c_str());
    });

    auto content = json.dump(2);
    THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), content.c_str(), gsl::narrow_cast<DWORD>(content.size()), nullptr, nullptr));
    cleanup.release();

    Registration.Write(Property::TerminalProfilePath, targetPath.c_str());
}
CATCH_LOG();

_Requires_exclusive_lock_held_(m_instanceLock)
void LxssUserSessionImpl::_CreateVm()
{
    ExecutionContext context(Context::CreateVm);

    if (!m_utilityVm)
    {

        // Return an error if a plugin failed to initialize or needs a newer WSL version.
        // Note: It's better to do this here instead of CreateInstanceForCurrentUser() because we
        // can return a proper error message with the plugin name since we have an execution context here.
        m_pluginManager.ThrowIfFatalPluginError();

        const auto userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
        auto config = _GetResultantConfig(userToken.get());

        // Initialize policies for the plugin interface.
        WSLVmCreationSettings userSettings{};
        WI_SetFlagIf(userSettings.CustomConfigurationFlags, WSLUserConfigurationCustomKernel, !config.KernelPath.empty());
        WI_SetFlagIf(userSettings.CustomConfigurationFlags, WSLUserConfigurationCustomKernelCommandLine, !config.KernelCommandLine.empty());

        // Duplicate the passed-in user token and pass it down to plugins.
        THROW_IF_WIN32_BOOL_FALSE(
            ::DuplicateTokenEx(userToken.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenImpersonation, &m_userToken));

        m_session.UserToken = m_userToken.get();

        GUID vmId{};
        THROW_IF_FAILED(CoCreateGuid(&vmId));

        m_vmId.store(vmId);

        // Create the utiliy VM and register for callbacks.
        m_utilityVm = WslCoreVm::Create(m_userToken, std::move(config), vmId);

        m_utilityVm->GetRuntimeId();

        if (m_httpProxyStateTracker)
        {
            // this needs to be done after the VM has finished in case we fell back to NAT mode
            m_httpProxyStateTracker->ConfigureNetworkingMode(m_utilityVm->GetConfig().NetworkingMode);
        }

        try
        {
            auto callback = [this](auto Pid) {
                // If the vm is currently being destroyed, the instance lock might be held
                // while WslCoreVm's destructor is waiting on this thread.
                // Cancel the call if the vm destruction is signaled.
                // Note: This is safe because m_instanceLock is always initialized
                // and because WslCoreVm's destructor waits for this thread, the session can't be gone
                // until this callback completes.

                auto lock = m_instanceLock.try_lock();
                while (!lock)
                {
                    if (m_vmTerminating.wait(100))
                    {
                        return;
                    }
                    lock = m_instanceLock.try_lock();
                }

                auto unlock = wil::scope_exit([&]() { m_instanceLock.unlock(); });
                TerminateByClientIdLockHeld(Pid);
            };

            m_utilityVm->RegisterCallbacks(std::bind(callback, _1), std::bind(s_VmTerminated, this, _1));

            // Mount disks after the system distro vhd is mounted in case filesystem detection is needed.
            _LoadDiskMounts();

            // Save the networking settings so they can be reused on the next instantiation.
            m_utilityVm->GetConfig().SaveNetworkingSettings(m_userToken.get());

            // If the telemetry is enabled, launch the telemetry agent inside the VM.
            if (m_utilityVm->GetConfig().EnableTelemetry && TraceLoggingProviderEnabled(g_hTraceLoggingProvider, WINEVENT_LEVEL_INFO, 0))
            {
                bool drvFsNotifications = false;
                {
                    auto impersonate = wil::impersonate_token(m_userToken.get());
                    const auto lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
                    drvFsNotifications = wsl::windows::common::registry::ReadDword(
                                             lxssKey.get(), LXSS_NOTIFICATIONS_KEY, LXSS_NOTIFICATION_DRVFS_PERF_DISABLED, 0) == 0;
                }

                LPCSTR Arguments[] = {LX_INIT_TELEMETRY_AGENT, nullptr};
                auto socket = m_utilityVm->CreateRootNamespaceProcess(LX_INIT_PATH, Arguments);
                m_telemetryThread = std::thread(&LxssUserSessionImpl::TelemetryWorker, this, std::move(socket), drvFsNotifications);
            }

            m_pluginManager.OnVmStarted(&m_session, &userSettings);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION_MSG("VM failed to start, shutting down.");

            _VmTerminate();
            throw;
        }
    }

    _VmCheckIdle();
}

_Requires_exclusive_lock_held_(m_instanceLock)
void LxssUserSessionImpl::_DeleteDistribution(_In_ const LXSS_DISTRO_CONFIGURATION& Configuration, _In_ ULONG Flags)
{
    std::lock_guard lock(m_instanceLock);
    _DeleteDistributionLockHeld(Configuration, Flags);
}

// Function signature of the API to remove WSLg start menu shortcuts.
HRESULT RemoveAppProvider(LPCWSTR);

_Requires_exclusive_lock_held_(m_instanceLock)
void LxssUserSessionImpl::_DeleteDistributionLockHeld(_In_ const LXSS_DISTRO_CONFIGURATION& Configuration, _In_ ULONG Flags) const
{
    THROW_HR_IF(E_UNEXPECTED, (WI_IsAnyFlagSet(Flags, ~LXSS_DELETE_DISTRO_FLAGS_ALL)));

    // For WSL1 distributions delete rootfs, temp, and the 9p socket.
    std::filesystem::path deletePath{};
    if (WI_IsFlagSet(Flags, LXSS_DELETE_DISTRO_FLAGS_ROOTFS))
    {
        deletePath = Configuration.BasePath / LXSS_ROOTFS_DIRECTORY;
        if (PathFileExistsW(deletePath.c_str()))
        {
            LOG_IF_FAILED(wil::RemoveDirectoryRecursiveNoThrow(deletePath.c_str()));
        }

        deletePath = Configuration.BasePath / LXSS_TEMP_DIRECTORY;
        if (PathFileExistsW(deletePath.c_str()))
        {
            LOG_IF_FAILED(wil::RemoveDirectoryRecursiveNoThrow(deletePath.c_str()));
        }

        deletePath = Configuration.BasePath / LXSS_PLAN9_UNIX_SOCKET;
        if (PathFileExistsW(deletePath.c_str()))
        {
            LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(deletePath.c_str()));
        }
    }

    // For WSL2 distributions, unmount and delete the VHD.
    if (WI_IsFlagSet(Flags, LXSS_DELETE_DISTRO_FLAGS_VHD) || WI_IsFlagSet(Flags, LXSS_DELETE_DISTRO_FLAGS_UNMOUNT))
    {
        if (PathFileExistsW(Configuration.VhdFilePath.c_str()))
        {
            if (m_utilityVm)
                try
                {
                    m_utilityVm->EjectVhd(Configuration.VhdFilePath.c_str());
                }
            CATCH_LOG()

            if (WI_IsFlagSet(Flags, LXSS_DELETE_DISTRO_FLAGS_VHD))
            {
                LOG_IF_WIN32_BOOL_FALSE(DeleteFileW(Configuration.VhdFilePath.c_str()));
            }
        }
    }

    if (WI_IsFlagSet(Flags, LXSS_DELETE_DISTRO_FLAGS_SHORTCUTS))
    {
        // Delete the shortcut icon, if any
        const auto shortcutIconPath = Configuration.BasePath / c_shortIconName;
        if (std::filesystem::exists(shortcutIconPath))
        {
            LOG_IF_WIN32_BOOL_FALSE_MSG(DeleteFileW(shortcutIconPath.c_str()), "Failed to delete %ls", shortcutIconPath.c_str());
        }

        // Remove start menu entry for the distribution, if any.
        if (Configuration.ShortcutPath.has_value())
        {
            LOG_IF_WIN32_BOOL_FALSE_MSG(
                DeleteFileW(Configuration.ShortcutPath->c_str()), "Failed to delete %ls", Configuration.ShortcutPath->c_str());
        }

        // Remove the terminal profile, if any.
        try
        {
            const auto lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
            const auto profile = DistributionRegistration::Open(lxssKey.get(), Configuration.DistroId).Read(Property::TerminalProfilePath);

            if (profile.has_value())
            {
                LOG_IF_WIN32_BOOL_FALSE_MSG(DeleteFileW(profile->c_str()), "Failed to delete %ls", profile->c_str());
            }
        }
        CATCH_LOG()
    }

    // Remove start menu shortcuts for WSLg applications.
    if (WI_IsFlagSet(Flags, LXSS_DELETE_DISTRO_FLAGS_WSLG_SHORTCUTS))
        try
        {
            const auto dllPath = wsl::windows::common::wslutil::GetBasePath() / WSLG_TS_PLUGIN_DLL;
            static LxssDynamicFunction<decltype(RemoveAppProvider)> removeAppProvider(dllPath.c_str(), "RemoveAppProvider");
            LOG_IF_FAILED(removeAppProvider(Configuration.Name.c_str()));
        }
    CATCH_LOG()

    // If the basepath is empty, delete it.
    if (std::filesystem::is_empty(Configuration.BasePath))
    {
        LOG_IF_WIN32_BOOL_FALSE_MSG(
            RemoveDirectory(Configuration.BasePath.c_str()), "Failed to delete %ls", Configuration.BasePath.c_str());
    }
}

_Requires_exclusive_lock_held_(m_instanceLock)
std::vector<DistributionRegistration> LxssUserSessionImpl::_EnumerateDistributions(
    _In_ HKEY LxssKey, _In_ bool ListAll, _In_ const std::optional<GUID>& Exclude)
{
    // Iterate through all subkeys looking for distributions.
    std::vector<DistributionRegistration> distributions;
    std::vector<GUID> orphanedDistributions;
    for (const auto& distro : wsl::windows::common::registry::EnumGuidKeys(LxssKey))
    {
        if (Exclude.has_value() && IsEqualGUID(Exclude.value(), distro.first))
        {
            continue;
        }

        // Validate that the distribution's package is still installed.
        if (!_ValidateDistro(LxssKey, &distro.first))
        {
            orphanedDistributions.push_back(distro.first);
            continue;
        }

        auto registration = DistributionRegistration::Open(LxssKey, distro.first);

        // Add the distribution to the list if the caller requested all, or if
        // it is installed or upgrading.
        const DWORD state = registration.Read(Property::State);
        if ((ListAll) || (state == LxssDistributionStateInstalled))
        {
            distributions.push_back(std::move(registration));
        }
    }

    // Unregister each orphaned distribution.
    for (GUID Distro : orphanedDistributions)
    {
        // TODO: This can fail if the registration is broken.
        auto configuration = s_GetDistributionConfiguration(DistributionRegistration::Open(LxssKey, Distro));
        _UnregisterDistributionLockHeld(LxssKey, configuration);
    }

    // Ensure that the default distribution is still valid.
    if (!orphanedDistributions.empty())
        try
        {
            _GetDefaultDistro(LxssKey);
        }
    CATCH_LOG()

    return distributions;
}

_Requires_lock_held_(m_instanceLock)
void LxssUserSessionImpl::_EnsureNotLocked(_In_ LPCGUID DistroGuid, const std::source_location& location)
{
    const auto found = std::find_if(m_lockedDistributions.begin(), m_lockedDistributions.end(), [&DistroGuid](const auto& entry) {
        return IsEqualGUID(entry.first, *DistroGuid);
    });

    THROW_HR_IF_MSG(
        E_ILLEGAL_STATE_CHANGE,
        (found != m_lockedDistributions.end()),
        "%hs, %hs:%u",
        location.function_name(),
        location.file_name(),
        location.line());
}

_Requires_exclusive_lock_held_(m_instanceLock)
GUID LxssUserSessionImpl::_GetDefaultDistro(_In_ HKEY LxssKey)
{
    ExecutionContext context(Context::GetDefaultDistro);

    GUID defaultDistroId = {};
    HRESULT result;
    try
    {
        const auto defaultDistro = DistributionRegistration::OpenDefault(LxssKey);

        THROW_HR_IF(WSL_E_DEFAULT_DISTRO_NOT_FOUND, !defaultDistro.has_value());

        // Ensure that the default distribution is valid.
        if (!_ValidateDistro(LxssKey, &defaultDistro->Id()))
        {
            // Delete the old default distribution.
            DistributionRegistration::DeleteDefault(LxssKey);

            const auto configuration = s_GetDistributionConfiguration(defaultDistro.value());
            _UnregisterDistributionLockHeld(LxssKey, configuration);

            // Validate remaining WSL distributions, if there are any remaining
            // set the first one found to the new default.
            const auto distros = _EnumerateDistributions(LxssKey);
            THROW_HR_IF(WSL_E_DEFAULT_DISTRO_NOT_FOUND, (distros.size() == 0));

            DistributionRegistration::SetDefault(LxssKey, distros[0]);
            defaultDistroId = distros[0].Id();
        }
        else
        {
            defaultDistroId = defaultDistro->Id();
        }

        result = S_OK;
    }
    catch (...)
    {
        result = WSL_E_DEFAULT_DISTRO_NOT_FOUND;
    }

    THROW_IF_FAILED(result);

    return defaultDistroId;
}

LONG LxssUserSessionImpl::_GetElfExitStatus(_In_ const LXSS_RUN_ELF_CONTEXT& Context)
{
    // Wait for the instance to terminate or the client process to exit.
    const wil::unique_handle clientProcess = wsl::windows::common::wslutil::OpenCallingProcess(GENERIC_READ | SYNCHRONIZE);
    THROW_HR_IF(E_ABORT, !wsl::windows::common::relay::InterruptableWait(Context.instanceTerminatedEvent.get(), {clientProcess.get()}));

    // Ensure that the process exited successfully. If the process encountered
    // an error, wait for the stderr worker thread and log the error message.
    LONG exitStatus;
    THROW_IF_NTSTATUS_FAILED(LxssClientInstanceGetExitStatus(Context.instanceHandle.get(), &exitStatus));

    return exitStatus;
}

wsl::core::Config LxssUserSessionImpl::_GetResultantConfig(_In_ const HANDLE userToken)
{
    const auto configFilePath = wsl::windows::common::helpers::GetWslConfigPath(userToken);
    // Open the config file (%userprofile%\.wslconfig).
    wsl::core::Config config(configFilePath.c_str(), userToken);

    _LoadNetworkingSettings(config, userToken);
    return config;
}

void LxssUserSessionImpl::_LoadDiskMount(_In_ HKEY Key, _In_ const std::wstring& LunStr) const
try
{
    // Get the disk path
    const auto path = wsl::windows::common::registry::ReadString(Key, nullptr, c_diskValueName);

    // Get the disk type; throw if unexpected type
    const auto diskType = static_cast<WslCoreVm::DiskType>(wsl::windows::common::registry::ReadDword(
        Key, nullptr, c_disktypeValueName, static_cast<DWORD>(WslCoreVm::DiskType::PassThrough)));

    THROW_HR_IF(E_UNEXPECTED, (diskType != WslCoreVm::DiskType::VHD && diskType != WslCoreVm::DiskType::PassThrough));

    // Attach the disk to the VM, reusing the same LUN if possible.
    //
    // N.B. The user token is not provided because the key that holds the disk
    // state can only be written by elevated users.
    auto lun = std::stoul(LunStr);
    m_utilityVm->AttachDisk(path.c_str(), diskType, lun, true, nullptr);

    // Restore each mount point.
    for (const auto& e : wsl::windows::common::registry::EnumKeys(Key, KEY_READ))
    {
        auto optionalValue = [&](std::wstring& storage, LPCWSTR name) -> LPCWSTR {
            try
            {
                storage = wsl::windows::common::registry::ReadString(e.second.get(), nullptr, name);
                return storage.c_str();
            }
            catch (...)
            {
                LOG_CAUGHT_EXCEPTION();
                return nullptr;
            }
        };

        std::wstring options;
        std::wstring type;

        // Get the mount name
        auto diskName = wsl::windows::common::registry::ReadString(e.second.get(), nullptr, c_mountNameValueName, L"");

        // If there was not a disk name stored, set it to the default generated name when mounting
        const auto result = m_utilityVm->MountDisk(
            path.c_str(),
            diskType,
            std::stoul(e.first),
            diskName.empty() ? nullptr : diskName.c_str(),
            optionalValue(type, c_typeValueName),
            optionalValue(options, c_optionsValueName));

        LOG_HR_IF_MSG(
            E_UNEXPECTED,
            result.Result != 0,
            "Failed to restore disk mount. Device: '%ls', Partition: '%ls', error: %i, step: %i",
            path.c_str(),
            e.first.c_str(),
            result.Result,
            result.Step);
    }

    return;
}
CATCH_LOG()

void LxssUserSessionImpl::_LoadNetworkingSettings(_Inout_ wsl::core::Config& config, _In_ HANDLE userToken)
try
{
    const auto autoProxyRequested = config.EnableAutoProxy;
    if (config.EnableAutoProxy)
    {
        if (SUCCEEDED(HttpProxyStateTracker::s_LoadWinHttpProxyMethods()))
        {
            if (!m_httpProxyStateTracker)
            {
                try
                {
                    m_httpProxyStateTracker =
                        std::make_shared<HttpProxyStateTracker>(config.InitialAutoProxyTimeout, userToken, config.NetworkingMode);
                }
                catch (...)
                {
                    LOG_CAUGHT_EXCEPTION_MSG("autoProxy failed to start");
                    config.EnableAutoProxy = false;
                }
            }
        }
        else
        {
            config.EnableAutoProxy = false;
        }
    }

    WSL_LOG(
        "AutoProxyEnabled",
        TraceLoggingValue(autoProxyRequested, "autoProxyRequested"),
        TraceLoggingValue(config.EnableAutoProxy, "autoProxyEnabled"));
}
CATCH_LOG();

void LxssUserSessionImpl::_LoadDiskMounts()
try
{
    const auto key = wsl::windows::common::registry::OpenOrCreateLxssDiskMountsKey(&m_userSid.Sid);
    for (const auto& e : wsl::windows::common::registry::EnumKeys(key.get(), KEY_READ))
    {
        _LoadDiskMount(e.second.get(), e.first);
    }

    // Clear the state from the registry now that the mounts have been loaded
    wsl::windows::common::registry::ClearSubkeys(key.get());
    return;
}
CATCH_LOG()

void LxssUserSessionImpl::_ProcessImportResultMessage(
    const LX_MINI_INIT_IMPORT_RESULT& Message,
    const gsl::span<gsl::byte> Span,
    HKEY LxssKey,
    LXSS_DISTRO_CONFIGURATION& Configuration,
    wsl::windows::service::DistributionRegistration& Registration)
{
    THROW_HR_IF(WSL_E_NOT_A_LINUX_DISTRO, !Message.ValidDistribution);

    if (Configuration.Name.empty())
    {
        THROW_HR_IF(WSL_E_DISTRIBUTION_NAME_NEEDED, Message.DefaultNameIndex <= 0);

        auto distributionName = wsl::shared::string::MultiByteToWide(wsl::shared::string::FromSpan(Span, Message.DefaultNameIndex));

        // Validate that name is valid, and doesn't conflict with existing distributions.
        s_ValidateDistroName(distributionName.c_str());
        _ValidateDistributionNameAndPathNotInUse(LxssKey, nullptr, distributionName.c_str(), Registration.Id());

        Configuration.Name = std::move(distributionName);
        Registration.Write(Property::Name, Configuration.Name.c_str());
    }

    if (Message.FlavorIndex > 0)
    {
        Configuration.Flavor = wsl::shared::string::MultiByteToWide(wsl::shared::string::FromSpan(Span, Message.FlavorIndex));
        Registration.Write(Property::Flavor, Configuration.Flavor.c_str());
    }

    if (Message.VersionIndex != 0)
    {
        Configuration.OsVersion = wsl::shared::string::MultiByteToWide(wsl::shared::string::FromSpan(Span, Message.VersionIndex));
        Registration.Write(Property::OsVersion, Configuration.OsVersion.c_str());
    }

    // Do not create start menu shortcut or terminal profiles for appx based distributions.
    if (Configuration.PackageFamilyName.empty())
    {
        auto impersonate = wil::CoImpersonateClient();

        Registration.Write(Property::Modern, 1);

        std::filesystem::path iconPath;
        const auto basePath = wsl::windows::common::wslutil::GetBasePath();

        if (Message.ShortcutIconIndex != 0)
        {
            iconPath = Configuration.BasePath / c_shortIconName;
            const wil::unique_handle icon{CreateFileW(iconPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            THROW_LAST_ERROR_IF(!icon);

            const auto iconData = Span.subspan(Message.ShortcutIconIndex, Message.ShortcutIconSize);
            THROW_IF_WIN32_BOOL_FALSE(WriteFile(icon.get(), iconData.data(), static_cast<DWORD>(iconData.size_bytes()), nullptr, nullptr));
        }
        else
        {
            iconPath = basePath / L"wsl.exe";
        }

        if (Message.GenerateShortcut)
        {
            _CreateDistributionShortcut(Configuration.Name.c_str(), iconPath.c_str(), (basePath / L"wsl.exe").c_str(), Registration);
        }

        // Generate a Windows Terminal profile, as long as the distribution didn't opt-out of it.
        if (Message.GenerateTerminalProfile)
        {
            if (Message.TerminalProfileIndex != 0)
            {
                const auto terminalProfileSpan = Span.subspan(Message.TerminalProfileIndex);
                const std::string_view terminalProfile(reinterpret_cast<const char*>(terminalProfileSpan.data()), Message.TerminalProfileSize);
                _CreateTerminalProfile(terminalProfile, iconPath, Configuration, Registration);
            }
            else
            {
                constexpr auto defaultProfile = R"(
                            {
                                "profiles": [{
                                "startingDirectory": "~"
                                }]
                            })";

                _CreateTerminalProfile(defaultProfile, iconPath, Configuration, Registration);
            }
        }
    }
}

LXSS_RUN_ELF_CONTEXT LxssUserSessionImpl::_RunElfBinary(
    _In_ LPCSTR CommandLine,
    _In_ LPCWSTR TargetDirectory,
    _In_ HANDLE ClientProcess,
    _In_opt_ HANDLE StdIn,
    _In_opt_ HANDLE StdOut,
    _In_opt_ HANDLE StdErr,
    _In_opt_count_(NumMounts) PLX_KMAPPATHS_ADDMOUNT Mounts,
    _In_opt_ ULONG NumMounts)
{
    GUID instanceId;
    THROW_IF_FAILED(CoCreateGuid(&instanceId));

    // If the caller did not provide stdin, stdout, or stderr handles use the nul device.
    wil::unique_hfile stdInLocal;
    if (!ARGUMENT_PRESENT(StdIn))
    {
        stdInLocal = wsl::windows::common::filesystem::OpenNulDevice(GENERIC_READ);
        StdIn = stdInLocal.get();
    }

    wil::unique_hfile stdOutLocal;
    if (!ARGUMENT_PRESENT(StdOut))
    {
        stdOutLocal = wsl::windows::common::filesystem::OpenNulDevice(GENERIC_WRITE);
        StdOut = stdOutLocal.get();
    }

    wil::unique_hfile stdErrLocal;
    if (!ARGUMENT_PRESENT(StdErr))
    {
        stdErrLocal = wsl::windows::common::filesystem::OpenNulDevice(GENERIC_WRITE);
        StdErr = stdErrLocal.get();
    }

    // Get the user and instance tokens.
    const auto userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
    const wil::unique_handle instanceToken(wsl::windows::common::security::CreateRestrictedToken(userToken.get()));

    // Open handles to the root directory and temp directory while impersonating
    // the client.
    wil::unique_hfile rootDirectory;
    wil::unique_hfile tempDirectory;
    {
        auto runAsUser = wil::impersonate_token(userToken.get());
        rootDirectory = wsl::windows::common::filesystem::OpenDirectoryHandle(TargetDirectory, true);
        const auto tempFolder = std::filesystem::path(TargetDirectory) / LXSS_TEMP_DIRECTORY;
        wsl::windows::common::filesystem::EnsureDirectory(tempFolder.c_str());
        const auto instanceIdString = wsl::shared::string::GuidToString<wchar_t>(instanceId);
        const auto tempPath = tempFolder / instanceIdString;
        tempDirectory = wsl::windows::common::filesystem::WipeAndOpenDirectory(tempPath.c_str());
    }

    // Create manual reset event that is signaled on instance termination
    LXSS_RUN_ELF_CONTEXT elfContext;
    elfContext.instanceTerminatedEvent.create(wil::EventOptions::ManualReset);
    THROW_LAST_ERROR_IF(!elfContext.instanceTerminatedEvent);

    // Create and initialize a job object for the instance.
    const wil::unique_handle instanceJob(CreateJobObjectW(nullptr, nullptr));
    THROW_LAST_ERROR_IF(!instanceJob);

    Security::InitializeInstanceJob(instanceJob.get());

    // Create a new instance with bsdtar as the init process to perform the extraction.
    LX_KINSTANCECREATESTART createParameters = {};
    createParameters.InstanceId = instanceId;
    createParameters.RootFsType = LXSS_FS_TYPE_TMPFS;
    createParameters.RootDirectoryHandle = HandleToULong(rootDirectory.get());
    createParameters.TempDirectoryHandle = HandleToULong(tempDirectory.get());
    createParameters.JobHandle = HandleToULong(instanceJob.get());
    createParameters.TokenHandle = HandleToULong(instanceToken.get());
    createParameters.InstanceTerminatedEventHandle = HandleToULong(elfContext.instanceTerminatedEvent.get());
    createParameters.NumPathsToMap = NumMounts;
    createParameters.PathsToMap = Mounts;

    // Format the kernel command line.
    std::string kernelCommandLine("init=");
    kernelCommandLine += CommandLine;
    createParameters.KernelCommandLine = kernelCommandLine.c_str();

    // Set up the file descriptors that will be passed to the init process.
    LX_KINIT_FILE_DESCRIPTOR initFileDescriptors[] = {
        {StdIn, LX_O_RDONLY, LX_FD_CLOEXEC}, {StdOut, LX_O_WRONLY, LX_FD_CLOEXEC}, {StdErr, LX_O_WRONLY, LX_FD_CLOEXEC}};

    createParameters.NumInitFileDescriptors = RTL_NUMBER_OF(initFileDescriptors);
    createParameters.InitFileDescriptors = initFileDescriptors;

    {
        // Acquire assign primary token privilege in order to pass the primary token for init process.
        auto revertPriv = wsl::windows::common::security::AcquirePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
        THROW_IF_NTSTATUS_FAILED(LxssClientInstanceCreate(&createParameters, &elfContext.instanceHandle));
    }

    // Start the instance.
    THROW_IF_NTSTATUS_FAILED(LxssClientInstanceStart(elfContext.instanceHandle.get(), ClientProcess));

    return elfContext;
}

_Requires_lock_held_(m_instanceLock)
std::shared_ptr<LxssRunningInstance> LxssUserSessionImpl::_RunningInstance(_In_ LPCGUID DistroGuid)
{
    _EnsureNotLocked(DistroGuid);
    const auto instance = m_runningInstances.find(*DistroGuid);
    if (instance != m_runningInstances.end())
    {
        return instance->second;
    }

    return nullptr;
}

LXSS_VM_MODE_SETUP_CONTEXT
LxssUserSessionImpl::_RunUtilityVmSetup(_In_ const LXSS_DISTRO_CONFIGURATION& Configuration, _In_ LX_MESSAGE_TYPE MessageType, ULONG ExportFlags, bool SetVersion)
{
    THROW_HR_IF(E_INVALIDARG, ((MessageType != LxMiniInitMessageImport) && (MessageType != LxMiniInitMessageExport) && (MessageType != LxMiniInitMessageImportInplace)));

    // Open the client process so the operation can be aborted if client exits.
    wil::unique_handle clientProcess = wsl::windows::common::wslutil::OpenCallingProcess(GENERIC_READ | SYNCHRONIZE);

    // Ensure that the Linux utility VM has been created.
    std::lock_guard lock(m_instanceLock);
    _CreateVm();

    WI_SetFlagIf(ExportFlags, LXSS_EXPORT_DISTRO_FLAGS_VERBOSE, SetVersion && m_utilityVm->GetConfig().SetVersionDebug);

    // Generate a GUID for the instance.
    GUID instanceId;
    THROW_IF_FAILED(CoCreateGuid(&instanceId));

    LXSS_VM_MODE_SETUP_CONTEXT context{};
    ULONG connectPort{};
    context.instance = m_utilityVm->CreateInstance(instanceId, Configuration, MessageType, 0, 0, 0, ExportFlags, &connectPort);

    // Establish the socket that will be used to transfer the tar file contents.
    context.tarSocket = wsl::windows::common::hvsocket::Connect(m_utilityVm->GetRuntimeId(), connectPort);
    context.errorSocket = wsl::windows::common::hvsocket::Connect(m_utilityVm->GetRuntimeId(), connectPort);
    WI_ASSERT(context.tarSocket.is_valid());

    return context;
}

void LxssUserSessionImpl::_SendDistributionRegisteredEvent(_In_ const LXSS_DISTRO_CONFIGURATION& Configuration) const
{
    WslOfflineDistributionInformation distributionInfo{};
    distributionInfo.Id = Configuration.DistroId;
    distributionInfo.Name = Configuration.Name.c_str();
    distributionInfo.PackageFamilyName = Configuration.PackageFamilyName.c_str();
    distributionInfo.Flavor = Configuration.Flavor.c_str();
    distributionInfo.Version = Configuration.OsVersion.c_str();
    m_pluginManager.OnDistributionRegistered(&m_session, &distributionInfo);
}

_Requires_lock_held_(m_instanceLock)
void LxssUserSessionImpl::_SetDistributionInstalled(_In_ HKEY LxssKey, _In_ const GUID& DistroGuid)
{
    // Mark the distribution as installed.
    auto registration = DistributionRegistration::Open(LxssKey, DistroGuid);
    registration.Write(Property::State, LxssDistributionStateInstalled);

    // Set this distribution as the default if there is not already a default
    // distribution.
    const auto defaultDistro = DistributionRegistration::OpenDefault(LxssKey);
    if (!defaultDistro.has_value())
    {
        DistributionRegistration::SetDefault(LxssKey, registration);
    }
}

_Requires_lock_not_held_(m_instanceLock)
bool LxssUserSessionImpl::_TerminateInstance(_In_ LPCGUID DistroGuid, _In_ bool CheckForClients)
{
    std::lock_guard lock(m_instanceLock);
    return _TerminateInstanceInternal(DistroGuid, CheckForClients);
}

_Requires_exclusive_lock_held_(m_instanceLock)
bool LxssUserSessionImpl::_TerminateInstanceInternal(_In_ LPCGUID DistroGuid, _In_ bool CheckForClients)
{
    ExecutionContext context(Context::TerminateDistro);

    // Look up an instance with the matching distro identifier. If the are no
    // more active clients, it is stopped and removed from the list.
    bool success = true;
    const auto instance = m_runningInstances.find(*DistroGuid);

    if (instance != m_runningInstances.end())
    {
        const auto clientKey = instance->second->GetLifetimeManagerId();
        if ((CheckForClients == false) || (!m_lifetimeManager.IsAnyProcessRegistered(clientKey)))
        {
            // Stop the instance and move it to a list of terminated instances.
            // This allows the instance destructor to run without the instance
            // lock held, and allows in-flight termination callbacks to complete.
            const bool force = !CheckForClients;
            try
            {
                success = instance->second->RequestStop(force);
            }
            CATCH_LOG()

            success = (success || force);
            if (success)
            {
                if (const auto* wslcoreInstance = dynamic_cast<WslCoreInstance*>(instance->second.get()); wslcoreInstance != nullptr)
                {
                    m_pluginManager.OnDistributionStopping(&m_session, wslcoreInstance->DistributionInformation());
                }

                instance->second->Stop();

                const auto clientId = instance->second->GetClientId();
                {
                    auto lock = m_terminatedInstanceLock.lock_exclusive();
                    m_terminatedInstances.push_back(std::move(instance->second));
                }

                m_lifetimeManager.RemoveCallback(clientKey);

                m_runningInstances.erase(instance);

                // If the instance that was terminated was a WSL2 instance,
                // check if the VM is now idle.
                if (clientId != LXSS_CLIENT_ID_INVALID)
                {
                    _VmCheckIdle();
                }
            }
        }
    }

    return success;
}

void LxssUserSessionImpl::_UpdateInit(_In_ const LXSS_DISTRO_CONFIGURATION& Configuration)
{
    // Only update the init binary once per-distro, per-session.
    auto lock = m_initUpdateLock.lock_exclusive();
    if (std::find(m_updatedInitDistros.begin(), m_updatedInitDistros.end(), Configuration.DistroId) == m_updatedInitDistros.end())
    {
        wsl::windows::common::filesystem::UpdateInit(Configuration.BasePath.c_str(), Configuration.Version);
        m_updatedInitDistros.emplace_back(Configuration.DistroId);
    }
}

HRESULT LxssUserSessionImpl::MountRootNamespaceFolder(_In_ LPCWSTR HostPath, _In_ LPCWSTR GuestPath, _In_ bool ReadOnly, _In_ LPCWSTR Name)
{
    std::lock_guard lock(m_instanceLock);
    RETURN_HR_IF(E_NOT_VALID_STATE, !m_utilityVm);

    m_utilityVm->MountRootNamespaceFolder(HostPath, GuestPath, ReadOnly, Name);
    return S_OK;
}

HRESULT LxssUserSessionImpl::CreateLinuxProcess(_In_opt_ const GUID* Distro, _In_ LPCSTR Path, _In_ LPCSTR* Arguments, _Out_ SOCKET* Socket)
{
    std::lock_guard lock(m_instanceLock);
    RETURN_HR_IF(E_NOT_VALID_STATE, !m_utilityVm);

    if (Distro == nullptr)
    {
        *Socket = m_utilityVm->CreateRootNamespaceProcess(Path, Arguments).release();
    }
    else
    {
        const auto distro = _RunningInstance(Distro);
        THROW_HR_IF(WSL_E_VM_MODE_INVALID_STATE, !distro);

        const auto wsl2Distro = dynamic_cast<WslCoreInstance*>(distro.get());
        THROW_HR_IF(WSL_E_WSL2_NEEDED, !wsl2Distro);

        *Socket = wsl2Distro->CreateLinuxProcess(Path, Arguments).release();
    }
    return S_OK;
}

_Requires_exclusive_lock_held_(m_instanceLock)
void LxssUserSessionImpl::_UnregisterDistributionLockHeld(_In_ HKEY LxssKey, _In_ const LXSS_DISTRO_CONFIGURATION& Configuration)
{
    ExecutionContext context(Context::UnregisterDistro);

    try
    {
        const auto removedDistroString = wsl::shared::string::GuidToString<wchar_t>(Configuration.DistroId);

        // Terminate any running instance of the distro and delete the distro.
        _TerminateInstanceInternal(&Configuration.DistroId);

        // Impersonate the user and delete the distro filesystem.
        {
            auto runAsUser = wil::CoImpersonateClient();
            _DeleteDistributionLockHeld(Configuration);
        }

        // Delete the distro registry key.
        wsl::windows::common::registry::DeleteKey(LxssKey, removedDistroString.c_str());
    }
    CATCH_LOG()
}

void LxssUserSessionImpl::_TimezoneUpdated()
try
{
    WSL_LOG("Received timezone change notification");

    // Update the timezone information for each running instance.
    std::lock_guard lock(m_instanceLock);
    std::for_each(m_runningInstances.begin(), m_runningInstances.end(), [&](auto& pair) { pair.second->UpdateTimezone(); });

    return;
}
CATCH_LOG()

_Requires_exclusive_lock_held_(m_instanceLock)
bool LxssUserSessionImpl::_ValidateDistro(_In_ HKEY LxssKey, _In_ LPCGUID DistroGuid)
{
    bool isValid = false;
    std::wstring packageFamilyName;
    try
    {
        // Ensure a subkey exists for the distribution.
        auto configuration = s_GetDistributionConfiguration(DistributionRegistration::Open(LxssKey, *DistroGuid));
        packageFamilyName = configuration.PackageFamilyName;

        // If there is no package family name associated with the distribution,
        // the user is responsible for unregistering the distribution.
        // Otherwise, ensure that the package is still installed. If the
        // package is installed ensure that the root file system is present.
        //
        // N.B. This covers the case where a package was uninstalled and
        //      reinstalled without the service being invoked.
        isValid = true;

        // TODO: Below block needs test coverage
        if (!packageFamilyName.empty())
        {
            std::filesystem::path localPath;
            PCWSTR path;
            if (WI_IsFlagClear(configuration.Flags, LXSS_DISTRO_FLAGS_VM_MODE))
            {
                localPath = configuration.BasePath / LXSS_ROOTFS_DIRECTORY;
                path = localPath.c_str();
            }
            else
            {
                path = configuration.VhdFilePath.c_str();
            }

            auto runAsUser = wil::CoImpersonateClient();

            // If the path is not found and the package is removed, then the distro can be considered to be uninstalled.
            // Only do this if the path is actually missing to prevent any accidental distro deletion if the store API
            // can't find the package for transient reasons.
            if (!PathFileExistsW(path) && !wsl::windows::common::helpers::IsPackageInstalled(packageFamilyName.c_str()))
            {
                isValid = false;
            }
        }
    }
    CATCH_LOG()

    if (!isValid)
    {
        WSL_LOG("ValidateDistributionFailed", TraceLoggingValue(packageFamilyName.c_str(), "packageFamilyName"));
    }

    return isValid;
}

void LxssUserSessionImpl::_ValidateDistributionNameAndPathNotInUse(
    _In_ HKEY LxssKey, _In_opt_ LPCWSTR Path, _In_opt_ LPCWSTR Name, const std::optional<GUID>& Exclude)
{
    // Use the cannonical path to compare distribution registration paths.
    // The cannonical path allows us to compare paths regardless of symlinks.
    //
    // Even with this, it's theoretically possible to use different drive mounts to have two paths
    // that will point to the same underlying folder. To catch this, we'd need to use BY_HANDLE_FILE_INFORMATION and compare file & volume indexes.
    // Unfortunately this is tricky because this doesn't work of the folder doesn't exist yet (or if a registered distribution's folder has been deleted).
    // For the sake of simplicity, this isn't implemented given that trying to double register a distribution will fail at the VHD creation step regardless.

    std::error_code error;
    std::filesystem::path canonicalPath;

    if (Path != nullptr)
    {
        canonicalPath = std::filesystem::weakly_canonical(Path, error);
        if (error)
        {
            LOG_WIN32(error.value());
        }
        else
        {
            Path = canonicalPath.c_str();
        }
    }

    // Ensure no existing distributions have the same name or install path.
    for (const auto& distro : _EnumerateDistributions(LxssKey, true, Exclude))
    {
        // Return an appropriate failure code for the two possible
        // conditions here:
        //
        //     1. The distribution is already registered successfully.
        //     2. The distribution is currently being registered or unregistered by another thread.

        LXSS_DISTRO_CONFIGURATION configuration{};
        try
        {
            configuration = s_GetDistributionConfiguration(distro);
        }
        catch (...)
        {
            // Don't break registration of new distro if one registration is invalid.
            LOG_CAUGHT_EXCEPTION();
            continue;
        }

        if (Name != nullptr && wsl::shared::string::IsEqual(Name, configuration.Name, true))
        {
            THROW_HR_MSG(
                (configuration.State == LxssDistributionStateInstalled) ? HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) : E_ILLEGAL_STATE_CHANGE,
                "%ls already registered (state = %d)",
                Name,
                configuration.State);
        }

        if (Path != nullptr)
        {
            auto canonicalDistroPath = std::filesystem::weakly_canonical(configuration.BasePath, error);
            if (error)
            {
                LOG_WIN32(error.value());
            }

            // Ensure another distribution by a different name is not already registered to the same location.
            THROW_HR_IF(
                HRESULT_FROM_WIN32(ERROR_FILE_EXISTS),
                wsl::windows::common::string::IsPathComponentEqual(error ? configuration.BasePath.native() : canonicalDistroPath.native(), Path));
        }
    }
}

_Requires_exclusive_lock_held_(m_instanceLock)
void LxssUserSessionImpl::_VmCheckIdle()
{
    // If the VM is idle, queue a timer to terminate the VM.
    // Otherwise, cancel any pending termination timers.
    //
    // N.B. A negative timeout means that the VM will continue running until it
    //      is terminated via wsl.exe --shutdown, or the service is stopped.
    if (_VmIsIdle())
    {
        const auto timeout = m_utilityVm->GetVmIdleTimeout();
        if (timeout >= 0)
        {
            auto dueTime = wil::filetime::from_int64(-wil::filetime_duration::one_millisecond * timeout);
            SetThreadpoolTimer(m_vmTerminationTimer.get(), &dueTime, 0, 0);
        }
    }
    else
    {
        SetThreadpoolTimer(m_vmTerminationTimer.get(), nullptr, 0, 0);
    }
}

void LxssUserSessionImpl::_VmIdleTerminate()
{
    std::lock_guard lock(m_instanceLock);
    if (_VmIsIdle())
    {
        WSL_LOG("StopVm");
        m_utilityVm->SaveAttachedDisksState();
        _VmTerminate();
    }
}

_Requires_exclusive_lock_held_(m_instanceLock)
bool LxssUserSessionImpl::_VmIsIdle()
{
    const auto found = std::find_if(m_runningInstances.begin(), m_runningInstances.end(), [&](auto& pair) {
        return (pair.second->GetClientId() != LXSS_CLIENT_ID_INVALID);
    });

    return (m_utilityVm && m_lockedDistributions.empty() && (found == m_runningInstances.end()));
}

_Requires_exclusive_lock_held_(m_instanceLock)
void LxssUserSessionImpl::_VmTerminate()
{
    // Cancel any pending termination timers and terminate the system distro and VM.
    SetThreadpoolTimer(m_vmTerminationTimer.get(), nullptr, 0, 0);

    if (m_utilityVm != nullptr)
    {
        m_pluginManager.OnVmStopping(&m_session);
    }

    m_vmTerminating.SetEvent();
    if (m_telemetryThread.joinable())
    {
        m_telemetryThread.join();
    }

    m_utilityVm.reset();
    m_vmId.store(GUID_NULL);

    // Reset the user's token since its lifetime is tied to the VM.
    m_userToken.reset();
    m_session.UserToken = nullptr;

    // Reset the event since the VM can be recreated.
    // This can done safely because WslCoreVm's destructor waits until
    // its distro exit callback is done before returning, so at this point
    // it's guaranteed that no one is waiting (or about to wait) on the event.
    // Note: Using an auto-reset event wouldn't work since the callback can be invoked
    // more than once while the vm is being destroyed.
    m_vmTerminating.ResetEvent();
}

void LxssUserSessionImpl::_SetHttpProxyInfo(std::vector<std::string>& environment) const noexcept
try
{
    // Copy guarantees a ref is held on the original instance, or it's null.
    const auto localTracker = m_httpProxyStateTracker;
    if (localTracker)
    {
        WSL_LOG("_SetHttpProxyInfo: Attempting to set proxy info");
        const std::optional<HttpProxySettings> proxySettings = localTracker->WaitForInitialProxySettings();

        if (proxySettings.has_value())
        {
            if (proxySettings->UnsupportedProxyDropReason != UnsupportedProxyReason::Supported)
            {
                switch (proxySettings->UnsupportedProxyDropReason)
                {
                case UnsupportedProxyReason::LoopbackNotMirrored:
                    EMIT_USER_WARNING(wsl::shared::Localization::MessageProxyLocalhostSettingsDropped());
                    break;
                case UnsupportedProxyReason::Ipv6NotMirrored:
                    EMIT_USER_WARNING(wsl::shared::Localization::MessageProxyV6SettingsDropped());
                    break;
                case UnsupportedProxyReason::LoopbackV6:
                    EMIT_USER_WARNING(wsl::shared::Localization::MessageProxyLoopbackV6SettingsDropped());
                    break;
                case UnsupportedProxyReason::UnsupportedError:
                    EMIT_USER_WARNING(wsl::shared::Localization::MessageProxyUnexpectedSettingsDropped());
                    break;
                case UnsupportedProxyReason::Supported:
                default:
                    WSL_LOG("_SetHttpProxyInfo: Unexpected UnsupportedProxyReason");
                }
            }
            if (proxySettings->HasSettingsConfigured())
            {
                s_AddHttpProxyToEnvironment(proxySettings.value(), environment);

                WSL_LOG(
                    "AutoProxyConfiguration",
                    TraceLoggingValue(!proxySettings->Proxy.empty(), "ProxySet"),
                    TraceLoggingValue(!proxySettings->SecureProxy.empty(), "SecureProxySet"),
                    TraceLoggingValue(proxySettings->ProxyBypasses.size(), "ProxyBypassesCount"),
                    TraceLoggingValue(!proxySettings->PacUrl.empty(), "PacUrlSet"));
            }
            else
            {
                WSL_LOG("_SetHttpProxyInfo: No HttpProxy settings detected so not configuring env vars.");
            }
        }
        else
        {
            // User will get a notification to restart WSL if proxy query completes later.
            WSL_LOG("_SetHttpProxyInfo: Initial HttpProxy query timeout, start WSL process anyway.");
        }
    }
}
CATCH_LOG()

void LxssUserSessionImpl::_LaunchOOBEIfNeeded() noexcept
try
{
    // Impersonate the user and open their lxss registry key.
    const wil::unique_hkey lxssKey = s_OpenLxssUserKey();

    // OOBE hasn't run if the value is not present or set to 0.
    if (wsl::windows::common::registry::ReadDword(lxssKey.get(), nullptr, LXSS_OOBE_COMPLETE_NAME, false) != false)
    {
        return;
    }

    // Don't run OOBE for existing users who already have a distro.
    wil::unique_cotaskmem_array_ptr<LXSS_ENUMERATE_INFO> distributions;
    THROW_IF_FAILED(EnumerateDistributions(distributions.size_address<ULONG>(), &distributions));
    if (distributions.size() > 1)
    {
        wsl::windows::common::registry::WriteDword(lxssKey.get(), nullptr, LXSS_OOBE_COMPLETE_NAME, true);
        return;
    }

    const auto userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
    // This is needed to launch the OOBE process as the user.
    wil::unique_handle userTokenCreateProcess;
    THROW_IF_WIN32_BOOL_FALSE(::DuplicateTokenEx(
        userToken.get(), MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenImpersonation, &userTokenCreateProcess));
    wsl::windows::common::helpers::LaunchWslSettingsOOBE(userTokenCreateProcess.get());
    wsl::windows::common::registry::WriteDword(lxssKey.get(), nullptr, LXSS_OOBE_COMPLETE_NAME, true);
}
CATCH_LOG()

LXSS_DISTRO_CONFIGURATION
LxssUserSessionImpl::s_GetDistributionConfiguration(const DistributionRegistration& Distro, bool skipName)
{
    ExecutionContext context(Context::ReadDistroConfig);

    // Read information about the distribution from the distro key.
    LXSS_DISTRO_CONFIGURATION configuration;
    configuration.DistroId = Distro.Id();
    configuration.State = Distro.Read(Property::State);
    configuration.Version = Distro.Read(Property::Version);
    configuration.BasePath = Distro.Read(Property::BasePath);
    configuration.PackageFamilyName = Distro.Read(Property::PackageFamilyName);

    // Read the vhd file name and append to the base path.
    configuration.VhdFilePath = configuration.BasePath / Distro.Read(Property::VhdFileName);
    configuration.Flags = Distro.Read(Property::Flags);

    configuration.OsVersion = Distro.Read(Property::OsVersion).value_or(L"");
    configuration.Flavor = Distro.Read(Property::Flavor).value_or(L"");
    configuration.RunOOBE = Distro.Read(Property::RunOOBE);
    configuration.ShortcutPath = Distro.Read(Property::ShortcutPath);

    if (!skipName)
    {
        configuration.Name = Distro.Read(Property::Name);
    }

    return configuration;
}

CreateLxProcessContext LxssUserSessionImpl::s_GetCreateProcessContext(_In_ const GUID& DistroGuid, _In_ bool SystemDistro)
{
    CreateLxProcessContext context{};
    std::vector<std::wstring> environment{};
    if (!SystemDistro)
    {
        auto runAsUser = wil::CoImpersonateClient();
        const auto lxssKey = wsl::windows::common::registry::OpenLxssUserKey();

        const auto registration = DistributionRegistration::Open(lxssKey.get(), DistroGuid);

        context.Flags = registration.Read(Property::Flags);
        context.DefaultEnvironment = registration.Read(Property::DefaultEnvironmnent);
    }
    else
    {
        context.Flags = DistributionRegistration::ApplyGlobalFlagsOverride(LXSS_DISTRO_FLAGS_DEFAULT | LXSS_DISTRO_FLAGS_VM_MODE);
        context.DefaultEnvironment = Property::DefaultEnvironmnent.DefaultValue;
    }

    context.UserToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
    context.Elevated = wsl::windows::common::security::IsTokenElevated(context.UserToken.get());
    return context;
}

// Note that if the user defines proxy variables via WSLENV, these values will be overwritten by those when init spawns process
void LxssUserSessionImpl::s_AddHttpProxyToEnvironment(_In_ const HttpProxySettings& proxySettings, _Inout_ std::vector<std::string>& environment) noexcept
try
{
    if (!proxySettings.Proxy.empty())
    {
        // Note that we add both lower and uppercase as some Linux apps use upper, others lower.
        environment.emplace_back(std::format("{}={}", c_httpProxyLower, proxySettings.Proxy));
        environment.emplace_back(std::format("{}={}", c_httpProxyUpper, proxySettings.Proxy));
    }

    if (!proxySettings.SecureProxy.empty())
    {
        environment.emplace_back(std::format("{}={}", c_httpsProxyLower, proxySettings.SecureProxy));
        environment.emplace_back(std::format("{}={}", c_httpsProxyUpper, proxySettings.SecureProxy));
    }

    if (!proxySettings.ProxyBypassesComma.empty())
    {
        environment.emplace_back(std::format("{}={}", c_proxyBypassLower, proxySettings.ProxyBypassesComma));
        environment.emplace_back(std::format("{}={}", c_proxyBypassUpper, proxySettings.ProxyBypassesComma));
    }

    if (!proxySettings.PacUrl.empty())
    {
        // We only add uppercase as there is no standard environment variable for PAC proxies.
        // This at least makes the PAC url available to the user in case they wish to use it.
        environment.emplace_back(std::format("{}={}", c_pacProxy, proxySettings.PacUrl));
    }
}
CATCH_LOG()

wil::unique_hkey LxssUserSessionImpl::s_OpenLxssUserKey()
{
    auto runAsUser = wil::CoImpersonateClient();
    return wsl::windows::common::registry::OpenLxssUserKey();
}

bool LxssUserSessionImpl::s_TerminateInstance(_Inout_ LxssUserSessionImpl* UserSession, _In_ GUID DistroGuid, _In_ bool CheckForClients)
{
    bool success = true;
    try
    {
        success = UserSession->_TerminateInstance(&DistroGuid, CheckForClients);
    }
    CATCH_LOG()

    return success;
}

void LxssUserSessionImpl::s_UpdateInit(_Inout_ LxssUserSessionImpl* UserSession, _In_ const LXSS_DISTRO_CONFIGURATION& Configuration)
try
{
    UserSession->_UpdateInit(Configuration);
}
CATCH_LOG()

LRESULT
CALLBACK
LxssUserSessionImpl::s_TimezoneWindowProc(HWND windowHandle, UINT messageCode, WPARAM wParameter, LPARAM lParameter)
{
    if (messageCode == WM_TIMECHANGE)
    {
        auto* session = reinterpret_cast<LxssUserSessionImpl*>(GetWindowLongPtr(windowHandle, GWLP_USERDATA));
        if (session != nullptr)
        {
            session->_TimezoneUpdated();
        }
    }

    return DefWindowProc(windowHandle, messageCode, wParameter, lParameter);
}

void LxssUserSessionImpl::s_ValidateDistroName(_In_ LPCWSTR Name)
{
    // Validate the name string. The name must match the regular expression
    // and cannot be the reserved legacy name.
    std::wstring regex{L"^[a-zA-Z0-9._-]{1,"};
    regex += std::to_wstring(LX_INIT_DISTRO_NAME_MAX);
    regex += L"}$";
    if ((!std::regex_match(Name, std::wregex(regex.c_str()))) || wsl::shared::string::IsEqual(Name, LXSS_LEGACY_INSTALL_NAME, true))
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, wsl::shared::Localization::MessageInvalidInstallDistributionName(Name));
    }
}

VOID CALLBACK LxssUserSessionImpl::s_VmIdleTerminate(_Inout_ PTP_CALLBACK_INSTANCE, _Inout_opt_ PVOID Context, _Inout_ PTP_TIMER)
{
    try
    {
        const auto userSession = reinterpret_cast<LxssUserSessionImpl*>(Context);
        userSession->_VmIdleTerminate();
    }
    CATCH_LOG()
}

void LxssUserSessionImpl::s_VmTerminated(_Inout_ LxssUserSessionImpl* UserSession, _In_ const GUID& VmId)
try
{
    UNREFERENCED_PARAMETER(VmId);

    UserSession->TerminateByClientId(LXSS_CLIENT_ID_WILDCARD);
    return;
}
CATCH_LOG()
