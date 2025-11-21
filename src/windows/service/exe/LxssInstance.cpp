/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    LxssInstance.cpp

Abstract:

    This file contains lxss instance definitions.

--*/

#include "precomp.h"
#include "LxssInstance.h"
#include "LxssSecurity.h"
#include "LxssUserSession.h"

// Number of milliseconds to wait for the init process to respond.
#define LXSS_INIT_CONNECTION_TIMEOUT_MS (30 * 1000)

// Registry keys related to integrity level checks.
#define LXSS_SERVICE_REGISTRY_INTEGRITY_CHECK L"DisableMixedIntegrityLaunch"
#define LXSS_SERVICE_REGISTRY_INTEGRITY_CHECK_DISABLED 0
#define LXSS_SERVICE_REGISTRY_INTEGRITY_CHECK_ENABLED 1

// Legacy folder mount information.
#define LXSS_CACHE_MOUNT_LXSS "/cache"
#define LXSS_CACHE_MOUNT_NT L"cache"
#define LXSS_CACHE_PERMISSIONS (0770)
#define LXSS_DATA_MOUNT_LXSS "/data"
#define LXSS_DATA_MOUNT_NT L"data"
#define LXSS_DATA_PERMISSIONS (0771)
#define LXSS_HOME_MOUNT_LXSS "/home"
#define LXSS_HOME_MOUNT_NT L"home"
#define LXSS_HOME_PERMISSIONS (0755)
#define LXSS_MNT_MOUNT_LXSS "/mnt"
#define LXSS_MNT_MOUNT_NT L"mnt"
#define LXSS_MNT_PERMISSIONS (0755)
#define LXSS_ROOT_HOME_MOUNT_LXSS "/root"
#define LXSS_ROOT_HOME_MOUNT_NT L"root"
#define LXSS_ROOT_PERMISSIONS (0700)
#define LXSS_ROOTFS_PERMISSIONS (0755)

extern bool g_lxcoreInitialized;

using namespace std::placeholders;
using namespace Microsoft::WRL;
using namespace wsl::windows::common::filesystem;

LxssInstance::LxssInstance(
    _In_ const GUID& InstanceId,
    _In_ const LXSS_DISTRO_CONFIGURATION& Configuration,
    _In_ ULONG DefaultUid,
    _In_ ULONG64 ClientLifetimeId,
    _In_ const std::function<void()>& TerminationCallback,
    _In_ const std::function<void()>& UpdateInitCallback,
    _In_ ULONG Flags,
    _In_ int IdleTimeout) :
    LxssRunningInstance(IdleTimeout),
    m_instanceId(InstanceId),
    m_instanceHandle(),
    m_terminationCallback(TerminationCallback),
    m_initialized(false),
    m_running(false),
    m_defaultUid(DefaultUid),
    m_configuration(Configuration),
    m_ntClientLifetimeId(ClientLifetimeId),
    m_instanceBasicIntegrityLevelCheckEnabled(false),
    m_redirectorConnectionTargets{m_configuration.Name}
{
    // Running a WSL1 distro is not possible if the lxcore driver is not present.
    THROW_HR_IF(WSL_E_WSL1_NOT_SUPPORTED, !g_lxcoreInitialized);

    // Copy immutable distribution data into the info structure.
    m_distributionInfo.Id = m_configuration.DistroId;
    m_distributionInfo.Name = m_configuration.Name.c_str();
    m_distributionInfo.PackageFamilyName = m_configuration.PackageFamilyName.c_str();
    m_distributionInfo.InitPid = 1;

    m_ServerPort = std::make_shared<LxssServerPort>();
    {
        // Create a job to hold pico processes from LXSS.
        auto runAsSessionUser = wil::CoImpersonateClient();

        m_instanceJob.reset(CreateJobObjectW(nullptr, nullptr));
        THROW_LAST_ERROR_IF(!m_instanceJob);

        Security::InitializeInstanceJob(m_instanceJob.get());

        // Check if VPN detection is enabled.
        const wil::unique_hkey LxssKey = wsl::windows::common::registry::OpenLxssUserKey();
        m_enableVpnDetection = (wsl::windows::common::registry::ReadDword(LxssKey.get(), nullptr, L"EnableVpnDetection", 1)) != 0;
    }

    // Store the user token for access checks.
    m_userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);

    // Create manual reset event that is signaled on instance termination
    m_instanceTerminatedEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    THROW_LAST_ERROR_IF(!m_instanceTerminatedEvent);

    // Check if integrity level check is enabled.
    //
    // N.B. The registry value is only writable from high-IL and above.
    if (wsl::windows::common::registry::ReadDword(
            HKEY_LOCAL_MACHINE, LXSS_SERVICE_REGISTRY_PATH, LXSS_SERVICE_REGISTRY_INTEGRITY_CHECK, LXSS_SERVICE_REGISTRY_INTEGRITY_CHECK_DISABLED) ==
        LXSS_SERVICE_REGISTRY_INTEGRITY_CHECK_ENABLED)
    {
        // Query the integrity level of the caller and store it in the instance.
        m_instanceBasicIntegrityLevel = wsl::windows::common::security::GetUserBasicIntegrityLevel(m_userToken.get());
        m_instanceBasicIntegrityLevelCheckEnabled = true;
    }

    // Initialize mount paths.
    _ConfigureFilesystem(Flags);

    // Update the init binary if needed.
    UpdateInitCallback();

    // Create the LXSS instance
    _StartInstance(Configuration.Flags);

    // Create a threadpool wait object to listen for instance termination
    m_terminationWait.reset(CreateThreadpoolWait(
        [](PTP_CALLBACK_INSTANCE, PVOID context, PTP_WAIT, TP_WAIT_RESULT) {
            try
            {
                const auto instance = static_cast<LxssInstance*>(context);
                instance->OnTerminated();
            }
            CATCH_LOG()
        },
        this,
        nullptr));

    THROW_LAST_ERROR_IF(!m_terminationWait);

    SetThreadpoolWait(m_terminationWait.get(), m_instanceTerminatedEvent.get(), nullptr);

    // Mark the instance as started.
    m_running = true;
}

LxssInstance::~LxssInstance()
{
    Stop();
}
bool CreateLxProcessIsValidStdHandle(_In_ PLXSS_HANDLE StdHandle)
{
    bool IsValid = false;
    switch (StdHandle->HandleType)
    {
    case LxssHandleConsole:
        if (StdHandle->Handle == LXSS_HANDLE_USE_CONSOLE)
        {
            IsValid = true;
        }

        break;

    case LxssHandleInput:
    case LxssHandleOutput:
        if (StdHandle->Handle != LXSS_HANDLE_USE_CONSOLE)
        {
            IsValid = true;
        }

        break;
    }

    return IsValid;
}

void LxssInstance::CreateLxProcess(
    _In_ const CreateLxProcessData& CreateProcessData,
    _In_ const CreateLxProcessContext& CreateProcessContext,
    _In_ const CreateLxProcessConsoleData& ConsoleData,
    _In_ SHORT Columns,
    _In_ SHORT Rows,
    _In_ PLXSS_STD_HANDLES StdHandles,
    _Out_ GUID* InstanceId,
    _Out_ HANDLE* ProcessHandle,
    _Out_ HANDLE* ServerHandle,
    _Out_ HANDLE* StandardIn,
    _Out_ HANDLE* StandardOut,
    _Out_ HANDLE* StandardErr,
    _Out_ HANDLE* CommunicationChannel,
    _Out_ HANDLE* InteropSocket)
{
    UNREFERENCED_PARAMETER(Columns);
    UNREFERENCED_PARAMETER(Rows);

    THROW_HR_IF(E_INVALIDARG, !CreateLxProcessIsValidStdHandle(&StdHandles->StdIn));
    THROW_HR_IF(E_INVALIDARG, !CreateLxProcessIsValidStdHandle(&StdHandles->StdOut));
    THROW_HR_IF(E_INVALIDARG, !CreateLxProcessIsValidStdHandle(&StdHandles->StdErr));

    // Check that the process is at the same basic integrity level if
    // needed.
    if (m_instanceBasicIntegrityLevelCheckEnabled != false)
    {
        const DWORD BasicIntegrityLevel =
            wsl::windows::common::security::GetUserBasicIntegrityLevel(CreateProcessContext.UserToken.get());
        if (m_instanceBasicIntegrityLevel != BasicIntegrityLevel)
        {
            if (m_instanceBasicIntegrityLevel > BasicIntegrityLevel)
            {
                THROW_HR(WSL_E_LOWER_INTEGRITY);
            }

            THROW_HR(WSL_E_HIGHER_INTEGRITY);
        }
    }

    if (m_oobeCompleteEvent && !m_oobeCompleteEvent.is_signaled())
    {
        EMIT_USER_WARNING(wsl::shared::Localization::MessageWaitingForOobe(m_configuration.Name.c_str()));
        m_oobeCompleteEvent.wait();
    }

    // Duplicate the handles from the calling process into the current process.
    std::vector<wil::unique_handle> StdHandlesLocal(3);
    if (StdHandles->StdIn.Handle != LXSS_HANDLE_USE_CONSOLE)
    {
        StdHandlesLocal[0].reset(wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(StdHandles->StdIn.Handle)));
    }

    if (StdHandles->StdOut.Handle != LXSS_HANDLE_USE_CONSOLE)
    {
        StdHandlesLocal[1].reset(wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(StdHandles->StdOut.Handle)));
    }

    if (StdHandles->StdErr.Handle != LXSS_HANDLE_USE_CONSOLE)
    {
        StdHandlesLocal[2].reset(wsl::windows::common::wslutil::DuplicateHandleFromCallingProcess(ULongToHandle(StdHandles->StdErr.Handle)));
    }

    // Enable symlink creation privilege on the token, which is needed
    // to allow DrvFs to create NT symlinks.
    //
    // N.B. This privilege is required to create NT symlinks only when
    //      developer mode is disabled. When developer mode is enabled,
    //      acquiring the privilege can fail, but creating the symlink
    //      will succeed even without the privilege. Therefore, it does
    //      not matter if this call fails.
    const wil::unique_handle Token = wsl::windows::common::security::GetUserToken(TokenPrimary);
    wsl::windows::common::security::EnableTokenPrivilege(Token.get(), SE_CREATE_SYMBOLIC_LINK_NAME);

    // Create an unnamed server port if the caller provided a buffer and
    // interop is enabled.
    wil::unique_handle ServerPort;
    PHANDLE ServerPortPointer = nullptr;
    if (LXSS_INTEROP_ENABLED(CreateProcessContext.Flags))
    {
        ServerPortPointer = &ServerPort;
    }

    // Send the create process request to the session leader.
    bool CreatedSessionLeader;
    const auto SessionLeader = std::static_pointer_cast<LxssMessagePort>(
        m_consoleManager->GetSessionLeader(ConsoleData, CreateProcessContext.Elevated, &CreatedSessionLeader));

    // If the session leader was just created, ensure the networking information
    // is up-to-date.
    if (CreatedSessionLeader)
    {
        _UpdateNetworkConfigurationFiles(true);
    }

    wil::unique_handle ProcessHandleLocal =
        _CreateLxProcess(SessionLeader, CreateProcessData, StdHandlesLocal, Token, m_defaultUid, ServerPortPointer);

    *InstanceId = m_instanceId;
    *ProcessHandle = ProcessHandleLocal.release();
    *ServerHandle = ServerPort ? ServerPort.release() : nullptr;
    *StandardIn = nullptr;
    *StandardOut = nullptr;
    *StandardErr = nullptr;
    *CommunicationChannel = nullptr;
    *InteropSocket = nullptr;
}

ULONG LxssInstance::GetClientId() const
{
    return LXSS_CLIENT_ID_INVALID;
}

GUID LxssInstance::GetDistributionId() const
{
    return m_configuration.DistroId;
}

std::shared_ptr<LxssPort> LxssInstance::GetInitPort()
{
    return m_InitMessagePort;
}

void LxssInstance::UpdateTimezone()
{
    const auto timezone = wsl::windows::common::helpers::GetLinuxTimezone(m_userToken.get());
    auto message = wsl::windows::common::helpers::GenerateTimezoneUpdateMessage(timezone);
    auto lock = m_InitMessagePort->Lock();
    m_InitMessagePort->Send(message.data(), gsl::narrow_cast<ULONG>(message.size()));
}

ULONG64 LxssInstance::GetLifetimeManagerId() const
{
    return m_ntClientLifetimeId;
}

void LxssInstance::Initialize()
{
    std::lock_guard<std::mutex> lock(m_stateLock);
    if (m_initialized)
    {
        return;
    }

    const auto socketPath = m_configuration.BasePath / LXSS_PLAN9_UNIX_SOCKET;

    // Open the communication channel with init.
    _InitiateConnectionToInitProcess();

    // Send initial configuration information to the init daemon.
    _InitializeConfiguration(socketPath);

    // Initialize networking for the instance, this will also register for network
    // state change notifications and send the initial network information to the
    // init process.
    _InitializeNetworking();

    // Initialization successful.
    m_initialized = true;
}

void LxssInstance::OnTerminated() const
{
    m_terminationCallback();
}

bool LxssInstance::RequestStop(_In_ bool Force)
{
    std::lock_guard<std::mutex> lock(m_stateLock);

    // Send the message to the init daemon to check if the instance can be terminated.
    bool shutdown = true;
    if (m_InitMessagePort)
    {
        try
        {
            auto lock = m_InitMessagePort->Lock();
            LX_INIT_TERMINATE_INSTANCE terminateMessage{};
            terminateMessage.Header.MessageType = LxInitMessageTerminateInstance;
            terminateMessage.Header.MessageSize = sizeof(terminateMessage);
            terminateMessage.Force = Force;
            m_InitMessagePort->Send(&terminateMessage, sizeof(terminateMessage));
            LX_INIT_TERMINATE_INSTANCE::TResponse terminateResponse{};
            m_InitMessagePort->Receive(&terminateResponse, sizeof(terminateResponse));
            shutdown = terminateResponse.Result;
        }
        CATCH_LOG()
    }

    return shutdown;
}

void LxssInstance::Stop()
{
    std::lock_guard<std::mutex> lock(m_stateLock);

    // Do nothing if the instance is already terminated.
    if (!m_running)
    {
        return;
    }

    // Logs when a distro is stopped
    WSL_LOG_TELEMETRY(
        "StopInstance",
        PDT_ProductAndServiceUsage,
        TraceLoggingKeyword(MICROSOFT_KEYWORD_CRITICAL_DATA),
        TraceLoggingValue(m_configuration.Name.c_str(), "distroName"),
        TraceLoggingValue(LXSS_WSL_VERSION_1, "version"),
        TraceLoggingValue(m_instanceId, "instanceId"));

    // Unregister the instance termination TP wait.
    if (m_terminationWait)
    {
        SetThreadpoolWait(m_terminationWait.get(), nullptr, nullptr);
    }

    // Unregister from network change notifications.
    m_networkNotificationHandle.reset();

    // Unwind in the reverse order of creation. Stop the LXSS instance, delete it.
    LOG_IF_NTSTATUS_FAILED(::LxssClientInstanceStop(m_instanceHandle.get()));
    m_instanceTerminatedEvent.reset();

    LOG_IF_NTSTATUS_FAILED(::LxssClientInstanceDestroy(m_instanceHandle.get()));
    m_instanceHandle.reset();

    // Wait on the oobe thread.
    if (m_oobeThread.joinable())
    {
        m_oobeThread.join();
    }

    // Remove the instance's Plan 9 Redirector connection targets.
    m_redirectorConnectionTargets.RemoveAll();

    // Attempt to clean up the instance's temp folder.
    if (!m_tempPath.empty())
    {
        auto runAsUser = wil::impersonate_token(m_userToken.get());
        LOG_IF_FAILED(wil::RemoveDirectoryRecursiveNoThrow(m_tempPath.c_str()));
    }

    m_running = false;
    m_tempPath.clear();
    m_rootDirectory.reset();
    m_tempDirectory.reset();
    return;
}

void LxssInstance::RegisterPlan9ConnectionTarget(_In_ HANDLE userToken)
{
    const auto path = m_configuration.BasePath / LXSS_PLAN9_UNIX_SOCKET;
    using unique_unicode_string = wil::unique_struct<UNICODE_STRING, decltype(RtlFreeUnicodeString), RtlFreeUnicodeString>;
    unique_unicode_string socketPath;
    THROW_IF_NTSTATUS_FAILED(RtlDosPathNameToNtPathName_U_WithStatus(path.c_str(), &socketPath, nullptr, nullptr));

    m_redirectorConnectionTargets.AddConnectionTarget(
        userToken, {}, m_defaultUid, std::wstring_view{socketPath.Buffer, socketPath.Length / sizeof(WCHAR)});
}

const WSLDistributionInformation* LxssInstance::DistributionInformation() const noexcept
{
    return &m_distributionInfo;
}

void LxssInstance::_ConfigureFilesystem(_In_ ULONG Flags)
{
    // Part of this process will try to upgrade existing LxFs folders to
    // enable the per-directory case sensitivity flag. To allow easy detection
    // of already processed folders, and resumption in case the process is
    // interrupted, directories are only marked case-sensitive after their
    // children are processed.
    //
    // Paths for LXSS instances look like so:
    //
    // <root>
    //      \rootfs               <-- Where the file system is located
    //      \temp\{Instance GUID} <-- Where temporary files go
    auto runAsUser = wil::CoImpersonateClient();

    // Ensure the parent temp folder exists and is empty.
    const auto tempFolder = m_configuration.BasePath / LXSS_TEMP_DIRECTORY;
    EnsureDirectory(tempFolder.c_str());
    LOG_IF_FAILED(wil::RemoveDirectoryRecursiveNoThrow(tempFolder.c_str(), wil::RemoveDirectoryOptions::KeepRootDirectory));

    // Create a subdirectory to be used as the temp folder for this instance.
    const auto instanceIdString = wsl::shared::string::GuidToString<wchar_t>(m_instanceId);
    m_tempPath = tempFolder / instanceIdString;

    // Make sure the directories of interest exist. Attributes are added
    // separately because on upgrade directories without attributes may
    // already exist.
    EnsureDirectory(m_configuration.BasePath.c_str());

    auto ensureDirectoryWithAttributes = [&](LPCWSTR directory, ULONG permissions) {
        EnsureDirectoryWithAttributes(
            (m_configuration.BasePath / directory).c_str(), permissions, LX_UID_ROOT, LX_GID_ROOT, Flags, m_configuration.Version);
    };

    ensureDirectoryWithAttributes(LXSS_ROOTFS_DIRECTORY, LXSS_ROOTFS_PERMISSIONS);

    // If this is the legacy distribution, ensure that the additional LxFs
    // directories exist and have the correct attributes. Otherwise, ensure that
    // the rootfs/mnt directory exists for DrvFs mounts.
    switch (m_configuration.Version)
    {
    case LXSS_DISTRO_VERSION_LEGACY:

        WI_ASSERT(IsEqualGUID(LXSS_LEGACY_DISTRO_GUID, m_configuration.DistroId));

        ensureDirectoryWithAttributes(LXSS_MNT_MOUNT_NT, LXSS_MNT_PERMISSIONS);
        ensureDirectoryWithAttributes(LXSS_CACHE_MOUNT_NT, LXSS_CACHE_PERMISSIONS);
        ensureDirectoryWithAttributes(LXSS_DATA_MOUNT_NT, LXSS_DATA_PERMISSIONS);
        ensureDirectoryWithAttributes(LXSS_ROOT_HOME_MOUNT_NT, LXSS_ROOTFS_PERMISSIONS);
        ensureDirectoryWithAttributes(LXSS_HOME_MOUNT_NT, LXSS_HOME_PERMISSIONS);
        break;

    case LXSS_DISTRO_VERSION_1:
    case LXSS_DISTRO_VERSION_CURRENT:
        break;

        DEFAULT_UNREACHABLE;
    }

    // Wipe out and then recreate the temporary directory.
    m_tempDirectory = WipeAndOpenDirectory(m_tempPath.c_str());

    // Open handle to rootfs directory for the instance.
    m_rootDirectory = OpenDirectoryHandle(m_configuration.BasePath.c_str(), true);
}

wil::unique_handle LxssInstance::_CreateLxProcess(
    _In_ const std::shared_ptr<LxssMessagePort>& MessagePort,
    _In_ const CreateLxProcessData& CreateProcessData,
    _In_ const std::vector<wil::unique_handle>& StdHandles,
    _In_ const wil::unique_handle& Token,
    _In_ ULONG DefaultUid,
    _Out_opt_ PHANDLE ServerPortHandle)
{
    auto lock = MessagePort->Lock();

    // Send a create process message for the session leader.
    auto Message = _CreateLxProcessMarshalMessage(MessagePort, CreateProcessData, StdHandles, Token, DefaultUid);

    WI_ASSERT(Message.size() <= ULONG_MAX);

    const auto MessageLocal = (PLX_INIT_CREATE_PROCESS)Message.data();
    const bool AllowOOBE = WI_IsFlagSet(MessageLocal->Common.Flags, LxInitCreateProcessFlagAllowOOBE);
    auto HandleEraser =
        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { _ReleaseHandlesFromLxProcessMarshalMessage(MessagePort, MessageLocal); });

    std::unique_ptr<LxssServerPort> ServerPort;
    if (ARGUMENT_PRESENT(ServerPortHandle) || AllowOOBE)
    {
        ServerPort = std::make_unique<LxssServerPort>(MessagePort->CreateUnnamedServer(&MessageLocal->IpcServerId));
    }

    MessagePort->Send(Message.data(), gsl::narrow_cast<ULONG>(Message.size()));

    if (AllowOOBE)
    {
        auto OobeMessagePort = ServerPort->WaitForConnection();

        {
            m_oobeCompleteEvent.create(wil::EventOptions::ManualReset);

            auto impersonate = wil::CoImpersonateClient();
            auto registration = wsl::windows::service::DistributionRegistration::Open(
                wsl::windows::common::registry::OpenLxssUserKey().get(), m_configuration.DistroId);

            // Wait for a potential previous oobe thread to complete before creating a new one.
            if (m_oobeThread.joinable())
            {
                m_oobeThread.join();
            }

            m_oobeThread = std::thread([this, OobeMessagePort = std::move(OobeMessagePort), registration = std::move(registration)]() mutable {
                try
                {
                    // N.B. The LX_INIT_OOBE_RESULT message is only sent once the OOBE process completes, which might be waiting on user input.
                    // Do no set a timeout here otherwise the OOBE flow will fail if the OOBE process takes longer than expected.
                    auto Message = OobeMessagePort->Receive(INFINITE);
                    auto* OobeResult = gslhelpers::try_get_struct<LX_INIT_OOBE_RESULT>(gsl::make_span(Message));
                    THROW_HR_IF(E_INVALIDARG, !OobeResult || (OobeResult->Header.MessageType != LxInitOobeResult));

                    WSL_LOG(
                        "OOBEResult",
                        TraceLoggingValue(OobeResult->Result, "Result"),
                        TraceLoggingValue(OobeResult->DefaultUid, "DefaultUid"),
                        TraceLoggingValue(m_configuration.Name.c_str(), "Name"),
                        TraceLoggingValue(1, "Version"));

                    if (OobeResult->Result == 0)
                    {
                        // OOBE was successful, don't run it again.
                        m_configuration.RunOOBE = false;
                        registration.Write(wsl::windows::service::Property::RunOOBE, 0);

                        if (OobeResult->DefaultUid != -1)
                        {
                            registration.Write(wsl::windows::service::Property::DefaultUid, static_cast<int>(OobeResult->DefaultUid));
                            m_defaultUid = static_cast<int>(OobeResult->DefaultUid);
                        }
                    }
                }
                CATCH_LOG()

                m_oobeCompleteEvent.SetEvent();
            });
        }
    }

    // Wait for the session leader to send the process identifier and unmarshal
    // the process handle.
    LXBUS_IPC_PROCESS_ID ProcessId = {};
    MessagePort->Receive(&ProcessId, sizeof(ProcessId));
    HandleEraser.release();

    auto ProcessEraser = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        // Reply to the init process that the process is not unmarshaled.
        ProcessId = 0;
        MessagePort->Send(&ProcessId, sizeof(ProcessId));
    });

    wil::unique_handle ProcessHandle = MessagePort->UnmarshalProcess(ProcessId);

    // Reply to the init process that the process is unmarshaled.
    ProcessId = 1;
    MessagePort->Send(&ProcessId, sizeof(ProcessId));

    ProcessEraser.release();

    if (ARGUMENT_PRESENT(ServerPortHandle))
    {
        *ServerPortHandle = ServerPort->ReleaseServerPort();
    }

    return ProcessHandle;
}

std::vector<gsl::byte> LxssInstance::_CreateLxProcessMarshalMessage(
    _In_ const std::shared_ptr<LxssMessagePort>& MessagePort,
    _In_ const CreateLxProcessData& CreateProcessData,
    _In_ const std::vector<wil::unique_handle>& StdHandles,
    _In_ const wil::unique_handle& Token,
    _In_ ULONG DefaultUid) const
{
    // Allocate a message and initialize the common parameters.
    auto Message = LxssCreateProcess::CreateMessage(LxInitMessageCreateProcess, CreateProcessData, DefaultUid);

    const auto MessageLocal = (PLX_INIT_CREATE_PROCESS)Message.data();

    {
        static_assert(LX_INIT_CREATE_PROCESS_USE_CONSOLE == 0);
    }

    {
        static_assert(LX_INIT_CREATE_PROCESS_USE_CONSOLE == LXSS_HANDLE_USE_CONSOLE);
    }

    // Marshal the standard handles.

    auto Eraser =
        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { _ReleaseHandlesFromLxProcessMarshalMessage(MessagePort, MessageLocal); });

    for (size_t Index = 0; Index < StdHandles.size(); ++Index)
    {
        if (StdHandles[Index])
        {
            LXBUS_IPC_MESSAGE_MARSHAL_HANDLE_DATA HandleData = {};
            HandleData.Handle = HandleToUlong(StdHandles[Index].get());
            if (Index == 0)
            {
                HandleData.HandleType = LxBusIpcMarshalHandleTypeInput;
            }
            else
            {
                HandleData.HandleType = LxBusIpcMarshalHandleTypeOutput;
            }

            MessageLocal->StdFdIds[Index] = MessagePort->MarshalHandle(&HandleData);
        }
    }

    // Marshal the token.

    {
        // Acquire assign primary token in order to pass the primary token for the new process.
        auto revertPriv = wsl::windows::common::security::AcquirePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
        MessageLocal->ForkTokenId = MessagePort->MarshalForkToken(Token.get());
    }

    if (m_configuration.RunOOBE && CreateProcessData.Filename.empty() && CreateProcessData.CommandLine.empty())
    {
        WI_SetFlag(MessageLocal->Common.Flags, LxInitCreateProcessFlagAllowOOBE);
    }

    Eraser.release();
    return Message;
}

void LxssInstance::_ReleaseHandlesFromLxProcessMarshalMessage(_In_ const std::shared_ptr<LxssMessagePort>& MessagePort, _In_ PLX_INIT_CREATE_PROCESS Message)
{
    if (Message->ForkTokenId != 0)
    {
        try
        {
            MessagePort->ReleaseForkToken(Message->ForkTokenId);
        }
        CATCH_LOG()
    }

    for (ULONG Index = 0; Index < RTL_NUMBER_OF(Message->StdFdIds); ++Index)
    {
        if (Message->StdFdIds[Index] != LXBUS_IPC_CONSOLE_ID_INVALID)
        {
            try
            {
                MessagePort->ReleaseHandle(Message->StdFdIds[Index]);
            }
            CATCH_LOG()
        }
    }
}

std::vector<unique_lxss_addmount> LxssInstance::_InitializeMounts() const
{
    // Legacy distributions have more than one LxFs mount. If this is a legacy
    // distribution add the /home, /root, /data, /cache, and /mnt LxFs mounts.
    std::vector<unique_lxss_addmount> mounts;
    auto addMount = [&](PCWSTR directory, PCWSTR source, LPCSTR target, ULONG mode) {
        mounts.emplace_back(CreateMount((m_configuration.BasePath / directory).c_str(), source, target, LXSS_FS_TYPE_LXFS, mode));
    };

    switch (m_configuration.Version)
    {
    case LXSS_DISTRO_VERSION_LEGACY:

        WI_ASSERT(IsEqualGUID(LXSS_LEGACY_DISTRO_GUID, m_configuration.DistroId));

        addMount(LXSS_ROOT_HOME_MOUNT_NT, LXSS_ROOT_HOME_MOUNT_NT, LXSS_ROOT_HOME_MOUNT_LXSS, LXSS_ROOT_PERMISSIONS);
        addMount(LXSS_HOME_MOUNT_NT, LXSS_HOME_MOUNT_NT, LXSS_HOME_MOUNT_LXSS, LXSS_HOME_PERMISSIONS);
        addMount(LXSS_DATA_MOUNT_NT, LXSS_DATA_MOUNT_NT, LXSS_DATA_MOUNT_LXSS, LXSS_DATA_PERMISSIONS);
        addMount(LXSS_CACHE_MOUNT_NT, LXSS_CACHE_MOUNT_NT, LXSS_CACHE_MOUNT_LXSS, LXSS_CACHE_PERMISSIONS);
        addMount(LXSS_MNT_MOUNT_NT, LXSS_MNT_MOUNT_NT, LXSS_MNT_MOUNT_LXSS, LXSS_MNT_PERMISSIONS);

    default:
        break;
    }

    return mounts;
}

void LxssInstance::_StartInstance(_In_ ULONG DistributionFlags)
{
    std::vector<unique_lxss_addmount> mounts;
    wil::unique_handle instanceToken;
    {
        // Be in the right session for creating the instance parameters.
        const auto userToken = wsl::windows::common::security::GetUserToken(TokenImpersonation);
        auto runAsUser = wil::impersonate_token(userToken.get());

        // Initialize mount points.
        mounts = _InitializeMounts();

        // Create token for the instance
        instanceToken = wsl::windows::common::security::CreateRestrictedToken(userToken.get());
    }

    // Create a new instance.
    LX_KINSTANCECREATESTART createParameters = {};
    createParameters.RootFsType = LXSS_DISTRO_USES_WSL_FS(m_configuration.Version) ? LXSS_FS_TYPE_WSLFS : LXSS_FS_TYPE_LXFS;
    WI_SetFlagIf(createParameters.Flags, LX_KINSTANCECREATESTART_FLAG_DISABLE_DRIVE_MOUNTING, WI_IsFlagClear(DistributionFlags, LXSS_DISTRO_FLAGS_ENABLE_DRIVE_MOUNTING));
    createParameters.InstanceId = m_instanceId;
    createParameters.RootDirectoryHandle = HandleToULong(m_rootDirectory.get());
    createParameters.TempDirectoryHandle = HandleToULong(m_tempDirectory.get());
    createParameters.JobHandle = HandleToULong(m_instanceJob.get());
    createParameters.TokenHandle = HandleToULong(instanceToken.get());
    createParameters.KernelCommandLine = LXSS_DISTRO_DEFAULT_KERNEL_COMMAND_LINE;
    createParameters.NumPathsToMap = static_cast<ULONG>(mounts.size());
    createParameters.PathsToMap = static_cast<PLX_KMAPPATHS_ADDMOUNT>(mounts.data());
    createParameters.InstanceTerminatedEventHandle = HandleToULong(m_instanceTerminatedEvent.get());
    const wil::unique_hfile nulDevice{OpenNulDevice(GENERIC_READ | GENERIC_WRITE)};
    LX_KINIT_FILE_DESCRIPTOR initFileDescriptors[] = {
        {nulDevice.get(), LX_O_RDONLY, 0}, {nulDevice.get(), LX_O_WRONLY, 0}, {nulDevice.get(), LX_O_WRONLY, 0}};

    createParameters.NumInitFileDescriptors = RTL_NUMBER_OF(initFileDescriptors);
    createParameters.InitFileDescriptors = initFileDescriptors;

    {
        // Acquire assign primary token in order to pass the primary token for init process.
        auto revertPriv = wsl::windows::common::security::AcquirePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
        THROW_IF_NTSTATUS_FAILED(::LxssClientInstanceCreate(&createParameters, &m_instanceHandle));
    }

    auto deleter = wil::scope_exit([&] { LOG_IF_NTSTATUS_FAILED(::LxssClientInstanceDestroy(m_instanceHandle.get())); });

    // Launch the instance.
    const wil::unique_handle clientProcess = wsl::windows::common::wslutil::OpenCallingProcess(PROCESS_CREATE_PROCESS | SYNCHRONIZE);
    THROW_IF_NTSTATUS_FAILED(::LxssClientInstanceStart(m_instanceHandle.get(), clientProcess.get()));

    deleter.release();
}

void LxssInstance::_UpdateNetworkInformation()
try
{
    // Impersonate the service.
    auto runAsSelf = wil::run_as_self();

    // Update the instance's DNS information.
    m_dnsInfo.UpdateNetworkInformation();

    // Update the resolv.conf file if it has changed.
    _UpdateNetworkConfigurationFiles(false);
    return;
}
CATCH_LOG()

void LxssInstance::_UpdateNetworkConfigurationFiles(_In_ bool UpdateAlways)
{
    // Generate contents of /etc/resolv.conf file
    wsl::core::networking::DnsSettingsFlags flags = wsl::core::networking::DnsSettingsFlags::IncludeIpv6Servers;
    WI_SetFlagIf(flags, wsl::core::networking::DnsSettingsFlags::IncludeVpn, m_enableVpnDetection);

    const auto dnsSettings = m_dnsInfo.GetDnsSettings(flags);
    std::string fileContents = GenerateResolvConf(dnsSettings);
    std::lock_guard<std::mutex> lock(m_resolvConfLock);
    if (!UpdateAlways && (fileContents == m_lastResolvConfContents))
    {
        return;
    }

    // Construct the network information message.
    wsl::shared::MessageWriter<LX_INIT_NETWORK_INFORMATION> message(LxInitMessageNetworkInformation);
    message.WriteString(message->FileHeaderIndex, wsl::shared::string::WideToMultiByte(LX_INIT_RESOLVCONF_FULL_HEADER));
    message.WriteString(message->FileContentsIndex, fileContents);
    auto messageSpan = message.Span();

    // Send the message.
    auto messagePortLock = m_InitMessagePort->Lock();
    m_InitMessagePort->Send(messageSpan.data(), gsl::narrow_cast<ULONG>(messageSpan.size()));
    m_lastResolvConfContents = std::move(fileContents);
}

void LxssInstance::_InitializeNetworking()
{
    m_ipTables.EnableIpTablesSupport(m_instanceHandle);

    // Register for network connectivity change notifications to update network information.
    LOG_IF_WIN32_ERROR(::NotifyNetworkConnectivityHintChange(
        [](PVOID context, NL_NETWORK_CONNECTIVITY_HINT) { static_cast<LxssInstance*>(context)->_UpdateNetworkInformation(); }, this, TRUE, &m_networkNotificationHandle));
}

void LxssInstance::_InitiateConnectionToInitProcess()
{
    // Register the server, wait for the init process to connect to the server,
    // and create the message event.
    m_ServerPort->RegisterLxBusServer(m_instanceHandle, LX_INIT_SERVER_NAME);
    std::unique_ptr<LxssMessagePort> NewMessagePort = m_ServerPort->WaitForConnection(LXSS_INIT_CONNECTION_TIMEOUT_MS);

    // Associate the server port with the new message port and create a console
    // manager that will be used to manage session leaders.
    NewMessagePort->SetServerPort(m_ServerPort);
    m_InitMessagePort = std::make_shared<LxssMessagePort>(std::move(NewMessagePort));
    m_consoleManager = ConsoleManager::CreateConsoleManager(m_InitMessagePort);
}

void LxssInstance::_InitializeConfiguration(_In_ const std::filesystem::path& Plan9SocketPath)
{
    // If DrvFs mounting is supported, initialize a bitmap of all fixed drives.
    ULONG fixedDrives = 0;
    if (WI_IsFlagSet(m_configuration.Flags, LXSS_DISTRO_FLAGS_ENABLE_DRIVE_MOUNTING))
    {
        fixedDrives = EnumerateFixedDrives().first;
    }

    const auto timezone = wsl::windows::common::helpers::GetLinuxTimezone(m_userToken.get());
    ULONG featureFlags{};
    WI_SetFlagIf(featureFlags, LxInitFeatureRootfsCompressed, WI_IsFlagSet(GetFileAttributesW(m_configuration.BasePath.c_str()), FILE_ATTRIBUTE_COMPRESSED));
    auto message = wsl::windows::common::helpers::GenerateConfigurationMessage(
        m_configuration.Name, fixedDrives, m_defaultUid, timezone, Plan9SocketPath.wstring(), featureFlags);

    // Send the message to the init daemon.
    auto lock = m_InitMessagePort->Lock();
    m_InitMessagePort->Send(message.data(), static_cast<ULONG>(message.size()));

    // Init replies with information about the distribution.
    auto buffer = m_InitMessagePort->Receive();
    const auto span = gsl::make_span(buffer);
    const auto* response = gslhelpers::try_get_struct<LX_INIT_CONFIGURATION_INFORMATION_RESPONSE>(span);
    THROW_HR_IF(E_UNEXPECTED, !response || response->Header.MessageType != LxInitMessageInitializeResponse);

    m_defaultUid = response->DefaultUid;
    if (response->VersionIndex > 0)
    {
        m_configuration.OsVersion = wsl::shared::string::MultiByteToWide(wsl::shared::string::FromSpan(span, response->VersionIndex));
        m_distributionInfo.Version = m_configuration.OsVersion.c_str();
    }

    if (response->FlavorIndex > 0)
    {
        m_configuration.Flavor = wsl::shared::string::MultiByteToWide(wsl::shared::string::FromSpan(span, response->FlavorIndex));
        m_distributionInfo.Flavor = m_configuration.Flavor.c_str();
    }
}
