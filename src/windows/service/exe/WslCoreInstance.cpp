/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreInstance.cpp

Abstract:

    This file contains WSL Core Instance function definitions.

--*/

#include "precomp.h"
#include "WslCoreInstance.h"

WslCoreInstance::WslCoreInstance(
    _In_ HANDLE UserToken,
    _In_ wil::unique_socket& InitSocket,
    _In_ wil::unique_socket& SystemDistroSocket,
    _In_ const GUID& InstanceId,
    _In_ const GUID& RuntimeId,
    _In_ const LXSS_DISTRO_CONFIGURATION& Configuration,
    _In_ ULONG DefaultUid,
    _In_ ULONG64 ClientLifetimeId,
    _In_ const std::function<LX_INIT_DRVFS_MOUNT(HANDLE)>& DrvFsCallback,
    _In_ ULONG FeatureFlags,
    _In_ DWORD SocketTimeout,
    _In_ int IdleTimeout,
    _Out_opt_ ULONG* ConnectPort) :
    LxssRunningInstance(IdleTimeout),
    m_featureFlags(FeatureFlags),
    m_instanceId(InstanceId),
    m_runtimeId(RuntimeId),
    m_configuration(Configuration),
    m_defaultUid(DefaultUid),
    m_initializeDrvFs(DrvFsCallback),
    m_ntClientLifetimeId(ClientLifetimeId),
    m_redirectorConnectionTargets{m_configuration.Name},
    m_socketTimeout(SocketTimeout)
{
    // Establish a communication channel with the init daemon.
    m_initChannel = std::make_shared<WslCorePort>(InitSocket.release(), m_runtimeId, m_socketTimeout);

    // Read a message from the init daemon. This will let us know if anything failed during startup.
    gsl::span<gsl::byte> span;
    const auto& result = m_initChannel->GetChannel().ReceiveMessage<LX_MINI_INIT_CREATE_INSTANCE_RESULT>(&span, m_socketTimeout);
    if (result.WarningsOffset != 0)
    {
        for (const auto& e : wsl::shared::string::Split<char>(wsl::shared::string::FromSpan(span, result.WarningsOffset), '\n'))
        {
            if (!e.empty())
            {
                EMIT_USER_WARNING(wsl::shared::string::MultiByteToWide(e));
            }
        }
    }

    if (result.Result != 0)
    {
        // N.B. EUCLEAN (117) can be returned if the disk's journal is corrupted.
        if ((result.Result == EINVAL || result.Result == 117) && result.FailureStep == LxInitCreateInstanceStepMountDisk)
        {
            THROW_HR(WSL_E_DISK_CORRUPTED);
        }
        else
        {
            THROW_HR_WITH_USER_ERROR(
                E_FAIL, wsl::shared::Localization::MessageDistributionFailedToStart(result.Result, static_cast<int>(result.FailureStep)));
        }
    }

    m_clientId = static_cast<ULONG>(result.Pid);
    if (ConnectPort != nullptr)
    {
        *ConnectPort = result.ConnectPort;
    }

    // Set a flag if the rootfs folder is compressed.
    //
    // N.B. The system distro has an empty base path.
    if (!m_configuration.BasePath.empty())
    {
        WI_SetFlagIf(m_featureFlags, LxInitFeatureRootfsCompressed, WI_IsFlagSet(GetFileAttributesW(m_configuration.BasePath.c_str()), FILE_ATTRIBUTE_COMPRESSED));
    }

    // Copy immutable distribution data into the info structure.
    m_distributionInfo.Id = m_configuration.DistroId;
    m_distributionInfo.Name = m_configuration.Name.c_str();
    m_distributionInfo.PackageFamilyName = m_configuration.PackageFamilyName.c_str();
    m_distributionInfo.InitPid = m_clientId;

    // Duplicate the passed-in user token.
    THROW_IF_WIN32_BOOL_FALSE(::DuplicateTokenEx(UserToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenImpersonation, &m_userToken));

    // If a system distro socket was provided, create a system distro for this instance.
    if (SystemDistroSocket)
    {
        LXSS_DISTRO_CONFIGURATION systemDistroConfig{};
        systemDistroConfig.DistroId = Configuration.DistroId;
        systemDistroConfig.State = LxssDistributionStateInstalled;
        systemDistroConfig.Version = LXSS_DISTRO_VERSION_2;
        systemDistroConfig.Flags = (LXSS_DISTRO_FLAGS_DEFAULT | LXSS_DISTRO_FLAGS_VM_MODE);

        // Allow interop requests from init (pid 1) and disable the 9p server.
        ULONG systemDistroFeatureFlags = m_featureFlags;
        WI_SetFlag(systemDistroFeatureFlags, LxInitFeatureDisable9pServer);
        WI_SetFlag(systemDistroFeatureFlags, LxInitFeatureSystemDistro);

        // Create an instance for the system distro, this will fail if the distro has opted-out of
        // GUI applications via /etc/wsl.conf.
        try
        {
            wil::unique_socket empty{};
            m_systemDistro = std::make_shared<WslCoreInstance>(
                UserToken,
                SystemDistroSocket,
                empty,
                WSL2_SYSTEM_DISTRO_GUID,
                RuntimeId,
                systemDistroConfig,
                LX_UID_ROOT,
                ClientLifetimeId,
                DrvFsCallback,
                systemDistroFeatureFlags,
                m_socketTimeout,
                IdleTimeout);
        }
        CATCH_LOG()
    }
}

WslCoreInstance::~WslCoreInstance()
{
    if (m_oobeThread.joinable())
    {
        m_destroyingEvent.SetEvent();
        m_oobeThread.join();
    }
}

void WslCoreInstance::CreateLxProcess(
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
    LX_INIT_DRVFS_MOUNT drvfsMount = LxInitDrvfsMountNone;
    // If drive mounting is supported, ensure that DrvFs has been initialized.
    if (WI_IsFlagSet(m_configuration.Flags, LXSS_DISTRO_FLAGS_ENABLE_DRIVE_MOUNTING))
    {
        drvfsMount = m_initializeDrvFs(CreateProcessContext.UserToken.get());
    }

    // Ensure the instance is still running.

    std::lock_guard lock(m_lock);
    THROW_HR_IF(HCS_E_TERMINATED, (!m_initChannel || !m_consoleManager));

    if (m_oobeCompleteEvent && !m_oobeCompleteEvent.is_signaled())
    {
        EMIT_USER_WARNING(wsl::shared::Localization::MessageWaitingForOobe(m_configuration.Name.c_str()));
        m_oobeCompleteEvent.wait();
    }

    // Initialize the create process message.
    // N.B. m_defaultUid can only be read after m_oobeCompleteEvent is signaled since OOBE can change the default UID.
    auto messageBuffer = LxssCreateProcess::CreateMessage(LxInitMessageCreateProcessUtilityVm, CreateProcessData, m_defaultUid);

    const auto messageSpan = gsl::make_span(messageBuffer);
    const auto message = gslhelpers::get_struct<LX_INIT_CREATE_PROCESS_UTILITY_VM>(messageSpan);

    // m_initializeDrvFs returns true if admin share should be used
    if (drvfsMount == LxInitDrvfsMountElevated && !m_adminMountNamespaceCreated)
    {
        MountDrvfs(true);
        m_adminMountNamespaceCreated = true;
    }
    else if (drvfsMount == LxInitDrvfsMountNonElevated && !m_nonAdminMountNamespaceCreated)
    {
        MountDrvfs(false);
        m_nonAdminMountNamespaceCreated = true;
    }

    message->Columns = Columns;
    message->Rows = Rows;
    WI_SetFlagIf(message->Common.Flags, LxInitCreateProcessFlagsStdInConsole, (StdHandles->StdIn.HandleType == LxssHandleConsole));
    WI_SetFlagIf(message->Common.Flags, LxInitCreateProcessFlagsStdOutConsole, (StdHandles->StdOut.HandleType == LxssHandleConsole));
    WI_SetFlagIf(message->Common.Flags, LxInitCreateProcessFlagsStdErrConsole, (StdHandles->StdErr.HandleType == LxssHandleConsole));
    WI_SetFlagIf(message->Common.Flags, LxInitCreateProcessFlagsElevated, (drvfsMount == LxInitDrvfsMountElevated));
    WI_SetFlagIf(message->Common.Flags, LxInitCreateProcessFlagsInteropEnabled, LXSS_INTEROP_ENABLED(CreateProcessContext.Flags));

    if (m_configuration.RunOOBE && CreateProcessData.Filename.empty() && CreateProcessData.CommandLine.empty())
    {
        WI_SetFlag(message->Common.Flags, LxInitCreateProcessFlagAllowOOBE);
    }

    // Create a session leader if needed.
    const auto sessionLeader =
        std::static_pointer_cast<WslCorePort>(m_consoleManager->GetSessionLeader(ConsoleData, CreateProcessContext.Elevated));

    // Lock the session leader connection and send a create process message.
    //
    // N.B. The session leader must be locked to ensure that the create process
    //      message and response are received by the correct endpoints.
    ULONG port;
    {
        auto sessionLock = sessionLeader->Lock();
        port = sessionLeader->GetChannel().Transaction<LX_INIT_CREATE_PROCESS_UTILITY_VM>(messageSpan).Result;
    }

    // Connect to the port specified by the session leader.
    std::vector<wil::unique_socket> sockets(LX_INIT_UTILITY_VM_CREATE_PROCESS_SOCKET_COUNT);
    if (WI_IsFlagSet(message->Common.Flags, LxInitCreateProcessFlagAllowOOBE))
    {
        sockets.emplace_back();
    }

    for (auto& socket : sockets)
    {
        socket = wsl::windows::common::hvsocket::Connect(m_runtimeId, port);
    }

    *InstanceId = m_runtimeId;
    *ProcessHandle = nullptr;
    *ServerHandle = nullptr;
    *StandardIn = reinterpret_cast<HANDLE>(sockets[0].release());
    *StandardOut = reinterpret_cast<HANDLE>(sockets[1].release());
    *StandardErr = reinterpret_cast<HANDLE>(sockets[2].release());
    *CommunicationChannel = reinterpret_cast<HANDLE>(sockets[3].release());
    *InteropSocket = reinterpret_cast<HANDLE>(sockets[4].release());

    if (WI_IsFlagSet(message->Common.Flags, LxInitCreateProcessFlagAllowOOBE))
    {
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

            m_oobeThread = std::thread([this, socket = std::move(sockets[5]), registration = std::move(registration)]() mutable {
                try
                {
                    ReadOOBEResult(std::move(socket), std::move(registration));
                }
                CATCH_LOG()

                m_oobeCompleteEvent.SetEvent();
            });
        }
    }
}

void WslCoreInstance::ReadOOBEResult(wil::unique_socket&& Socket, wsl::windows::service::DistributionRegistration&& registration)
{
    wsl::shared::SocketChannel channel(std::move(Socket), "OOBE", m_destroyingEvent.get());

    const auto* oobeResult = channel.ReceiveMessageOrClosed<LX_INIT_OOBE_RESULT>().first;

    if (oobeResult == nullptr)
    {
        LOG_HR_MSG(E_FAIL, "OOBE channel closed");
        return;
    }

    // Logs the result of the OOBE process
    WSL_LOG_TELEMETRY(
        "OOBEResult",
        PDT_ProductAndServicePerformance,
        TraceLoggingValue(oobeResult->Result, "Result"),
        TraceLoggingValue(oobeResult->DefaultUid, "DefaultUid"),
        TraceLoggingValue(m_configuration.Name.c_str(), "Name"),
        TraceLoggingValue(2, "Version"));

    if (oobeResult->Result == 0)
    {
        // OOBE was successful, don't run it again.
        m_configuration.RunOOBE = false;
        registration.Write(wsl::windows::service::Property::RunOOBE, 0);

        if (oobeResult->DefaultUid != -1)
        {
            registration.Write(wsl::windows::service::Property::DefaultUid, static_cast<int>(oobeResult->DefaultUid));
            m_defaultUid = static_cast<int>(oobeResult->DefaultUid);
        }
    }
}

ULONG WslCoreInstance::GetClientId() const
{
    // Return the system distro ClientId if any so that this distribution is correctly
    // identified if the system distro init process terminates.
    if (m_systemDistro)
    {
        return m_systemDistro->GetClientId();
    }

    return m_clientId;
}

GUID WslCoreInstance::GetDistributionId() const
{
    return m_configuration.DistroId;
}

std::shared_ptr<LxssPort> WslCoreInstance::GetInitPort()
{
    THROW_HR_IF(HCS_E_TERMINATED, !m_initChannel);

    return m_initChannel;
}

std::shared_ptr<LxssRunningInstance> WslCoreInstance::GetSystemDistro()
{
    return m_systemDistro;
}

void WslCoreInstance::UpdateTimezone()
{
    if (m_systemDistro)
    {
        m_systemDistro->UpdateTimezone();
    }

    auto message =
        wsl::windows::common::helpers::GenerateTimezoneUpdateMessage(wsl::windows::common::helpers::GetLinuxTimezone(m_userToken.get()));

    auto lock = m_initChannel->Lock();
    m_initChannel->GetChannel().SendMessage<LX_INIT_TIMEZONE_INFORMATION>(gsl::make_span(message));
}

ULONG64 WslCoreInstance::GetLifetimeManagerId() const
{
    return m_ntClientLifetimeId;
}

void WslCoreInstance::Initialize()
{
    // Check if the instance has already been initialized.
    std::lock_guard lock(m_lock);
    if (m_initialized)
    {
        return;
    }

    // If a system distro was created, initialize it first.
    if (m_systemDistro)
    {
        m_systemDistro->Initialize();
    }

    LX_INIT_DRVFS_MOUNT drvfsMount = LxInitDrvfsMountNone;

    // If drive mounting is supported, ensure that DrvFs has been initialized.
    if (WI_IsFlagSet(m_configuration.Flags, LXSS_DISTRO_FLAGS_ENABLE_DRIVE_MOUNTING))
    {
        drvfsMount = m_initializeDrvFs(m_userToken.get());
    }

    // Create a console manager that will be used to manage session leaders.
    m_consoleManager = ConsoleManager::CreateConsoleManager(m_initChannel);

    // Send the initial configuration information to the init daemon.
    ULONG fixedDrives = 0;
    if (WI_IsFlagSet(m_configuration.Flags, LXSS_DISTRO_FLAGS_ENABLE_DRIVE_MOUNTING))
    {
        fixedDrives = wsl::windows::common::filesystem::EnumerateFixedDrives().first;
    }

    const auto timezone = wsl::windows::common::helpers::GetLinuxTimezone();
    auto config = wsl::windows::common::helpers::GenerateConfigurationMessage(
        m_configuration.Name, fixedDrives, m_defaultUid, timezone, {}, m_featureFlags, drvfsMount);

    m_initChannel->GetChannel().SendMessage<LX_INIT_CONFIGURATION_INFORMATION>(gsl::span(config));

    // Init replies with information about the distribution.
    gsl::span<gsl::byte> span;
    const auto& response = m_initChannel->GetChannel().ReceiveMessage<LX_INIT_CONFIGURATION_INFORMATION_RESPONSE>(&span);
    m_defaultUid = response.DefaultUid;
    m_plan9Port = response.Plan9Port;
    m_distributionInfo.PidNamespace = response.PidNamespace;

    if (response.VersionIndex > 0)
    {
        m_configuration.OsVersion = wsl::shared::string::MultiByteToWide(wsl::shared::string::FromSpan(span, response.VersionIndex));
        m_distributionInfo.Version = m_configuration.OsVersion.c_str();
    }

    if (response.FlavorIndex > 0)
    {
        m_configuration.Flavor = wsl::shared::string::MultiByteToWide(wsl::shared::string::FromSpan(span, response.FlavorIndex));
        m_distributionInfo.Flavor = m_configuration.Flavor.c_str();
    }

    // Launch the interop server with the user's token.
    if (response.InteropPort != LX_INIT_UTILITY_VM_INVALID_PORT)
        try
        {
            const wil::unique_socket socket{wsl::windows::common::hvsocket::Connect(m_runtimeId, response.InteropPort)};
            wil::unique_handle info{wsl::windows::common::helpers::LaunchInteropServer(
                nullptr, reinterpret_cast<HANDLE>(socket.get()), nullptr, nullptr, &m_runtimeId, m_userToken.get())};
        }
    CATCH_LOG()

    // Initialization was successful.
    m_initialized = true;

    // The initialization message mounts the drvfs drives, so make sure we don't try it again.
    if (drvfsMount == LxInitDrvfsMountElevated)
    {
        m_adminMountNamespaceCreated = true;
    }
    else if (drvfsMount == LxInitDrvfsMountNonElevated)
    {
        m_nonAdminMountNamespaceCreated = true;
    }

    WSL_LOG(
        "WslCoreInstanceInitialize",
        TraceLoggingValue(m_configuration.Name.c_str(), "distroName"),
        TraceLoggingValue(LXSS_WSL_VERSION_2, "version"),
        TraceLoggingValue(m_instanceId, "instanceId"),
        TraceLoggingValue(m_configuration.DistroId, "distroId"),
        TraceLoggingValue(response.DefaultUid, "defaultUid"),
        TraceLoggingValue(response.SystemdEnabled, "systemdEnabled"));
}

void WslCoreInstance::MountDrvfs(bool Admin) const
{
    auto [drives, nonReadableDrives] = wsl::windows::common::filesystem::EnumerateFixedDrives();
    LX_INIT_MOUNT_DRVFS Message{{LxInitMessageRemountDrvfs, sizeof(Message)}, Admin, drives, nonReadableDrives, static_cast<int>(m_defaultUid)};

    const auto& Result = m_initChannel->GetChannel().Transaction(Message, nullptr, m_socketTimeout);

    LOG_HR_IF_MSG(E_UNEXPECTED, Result.Result != 0, "Failed to mount the drvfs shares, %i", Result.Result);
}

const WSLDistributionInformation* WslCoreInstance::DistributionInformation() const noexcept
{
    return &m_distributionInfo;
}

bool WslCoreInstance::RequestStop(_In_ bool Force)
{
    bool shutdown = true;
    std::lock_guard lock(m_lock);
    if (m_initChannel)
        try
        {
            LX_INIT_TERMINATE_INSTANCE terminateMessage{};
            terminateMessage.Header.MessageType = LxInitMessageTerminateInstance;
            terminateMessage.Header.MessageSize = sizeof(terminateMessage);
            terminateMessage.Force = Force;

            m_initChannel->GetChannel().SendMessage(terminateMessage);
            auto [message, span] = m_initChannel->GetChannel().ReceiveMessageOrClosed<RESULT_MESSAGE<bool>>(m_socketTimeout);
            if (message)
            {
                shutdown = message->Result;
            }
        }
    CATCH_LOG()

    return shutdown;
}

void WslCoreInstance::Stop()
{
    std::lock_guard lock(m_lock);

    WSL_LOG_TELEMETRY(
        "StopInstance",
        PDT_ProductAndServiceUsage,
        TraceLoggingKeyword(MICROSOFT_KEYWORD_CRITICAL_DATA),
        TraceLoggingValue(m_configuration.Name.c_str(), "distroName"),
        TraceLoggingValue(LXSS_WSL_VERSION_2, "version"),
        TraceLoggingValue(m_instanceId, "instanceId"),
        TraceLoggingValue(m_configuration.DistroId, "distroId"));

    m_destroyingEvent.SetEvent();
    m_initChannel.reset();
    m_consoleManager.reset();

    // Remove the instance's Plan 9 Redirector connection targets.
    m_redirectorConnectionTargets.RemoveAll();

    // If the instance was terminated, terminate the associated system distro.
    m_systemDistro.reset();

    return;
}

void WslCoreInstance::RegisterPlan9ConnectionTarget(_In_ HANDLE userToken)
{
    // If Plan 9 is running, add a connection target to the P9Rdr driver.
    if (m_plan9Port != LX_INIT_UTILITY_VM_INVALID_PORT)
    {
        m_redirectorConnectionTargets.AddConnectionTarget(userToken, {}, m_defaultUid, {}, m_runtimeId, m_plan9Port);
    }
}

wil::unique_socket WslCoreInstance::CreateLinuxProcess(_In_ LPCSTR Path, _In_ LPCSTR* Arguments)
{
    std::lock_guard lock(m_lock);

    return LxssCreateProcess::CreateLinuxProcess(Path, Arguments, m_runtimeId, m_initChannel->GetChannel(), nullptr, m_socketTimeout);
}

WslCoreInstance::WslCorePort::WslCorePort(_In_ SOCKET Socket, _In_ const GUID& RuntimeId, DWORD SocketTimeout) :
    m_channel(wil::unique_socket{Socket}, "WslCorePort"), m_runtimeId(RuntimeId), m_socketTimeout(SocketTimeout)

{
    // N.B. The class takes ownership of the socket.
}

std::shared_ptr<LxssPort> WslCoreInstance::WslCorePort::CreateSessionLeader(_In_ HANDLE)
{
    // Send a create session message to the init daemon.
    // N.B. A lock is held while this method is called.

    LX_INIT_CREATE_SESSION message{{LxInitMessageCreateSession, sizeof(message)}};
    const auto& response = m_channel.Transaction(message, nullptr, m_socketTimeout);

    wil::unique_socket socket = wsl::windows::common::hvsocket::Connect(m_runtimeId, response.Port);
    return std::make_shared<WslCorePort>(socket.release(), m_runtimeId, m_socketTimeout);
}

void WslCoreInstance::WslCorePort::DisconnectConsole(_In_ HANDLE)
{
}

wsl::shared::SocketChannel& WslCoreInstance::WslCorePort::GetChannel()
{
    return m_channel;
}

wil::cs_leave_scope_exit WslCoreInstance::WslCorePort::Lock()
{
    return m_lock.lock();
}

void WslCoreInstance::WslCorePort::Receive(_Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length, _In_opt_ HANDLE ClientProcess, _In_ DWORD Timeout)
{
    const auto span = gsl::make_span(reinterpret_cast<gsl::byte*>(Buffer), Length);
    const ULONG bytesRead = wsl::windows::common::socket::Receive(m_channel.Socket(), span, ClientProcess, MSG_WAITALL, Timeout);

    THROW_HR_IF_MSG(E_UNEXPECTED, bytesRead < Length, "Expected %lu bytes, but received %lu", Length, bytesRead);
}

void WslCoreInstance::WslCorePort::Send(_In_reads_bytes_(Length) PVOID Buffer, _In_ ULONG Length)
{
    wsl::windows::common::socket::Send(m_channel.Socket(), gsl::make_span(reinterpret_cast<gsl::byte*>(Buffer), Length));
}
