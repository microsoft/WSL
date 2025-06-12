/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslCoreInstance.h

Abstract:

    This file contains WSL Core Instance function declarations.

--*/

#pragma once

#include "precomp.h"
#include "hcs.hpp"
#include "LxssPort.h"
#include "LxssCreateProcess.h"
#include "LxssConsoleManager.h"
#include "WslPluginApi.h"
#include "DistributionRegistration.h"

class WslCoreInstance : public LxssRunningInstance
{
public:
    class WslCorePort final : public LxssPort
    {
    public:
        WslCorePort(_In_ SOCKET Socket, _In_ const GUID& RuntimeId, _In_ DWORD SocketTimeout);

        WslCorePort(const WslCorePort&) = delete;
        WslCorePort& operator=(const WslCorePort&) = delete;
        WslCorePort(WslCorePort&& Source) = delete;
        WslCorePort& operator=(WslCorePort&& Source) = delete;

        std::shared_ptr<LxssPort> CreateSessionLeader(_In_ HANDLE ClientProcess) override;
        void DisconnectConsole(_In_ HANDLE ClientProcess) override;
        wsl::shared::SocketChannel& GetChannel();
        wil::cs_leave_scope_exit Lock() override;
        void Receive(_Out_writes_bytes_(Length) PVOID Buffer, _In_ ULONG Length, _In_opt_ HANDLE ClientProcess = nullptr, _In_ DWORD Timeout = INFINITE) override;
        void Send(_In_reads_bytes_(Length) PVOID Buffer, _In_ ULONG Length) override;

    private:
        wil::critical_section m_lock;
        wsl::shared::SocketChannel m_channel;
        GUID m_runtimeId{};
        DWORD m_socketTimeout{};
    };

    WslCoreInstance(const WslCoreInstance&) = delete;
    WslCoreInstance(WslCoreInstance&&) = delete;
    WslCoreInstance& operator=(const WslCoreInstance&) = delete;
    WslCoreInstance& operator=(WslCoreInstance&&) = delete;

    WslCoreInstance(
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
        _Out_opt_ ULONG* ConnectPort = nullptr);

    virtual ~WslCoreInstance();

    void CreateLxProcess(
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
        _Out_ HANDLE* InteropSocket) override;

    ULONG GetClientId() const override;

    GUID GetDistributionId() const override;

    std::shared_ptr<LxssPort> GetInitPort() override;

    std::shared_ptr<LxssRunningInstance> GetSystemDistro();

    void UpdateTimezone() override;

    ULONG64 GetLifetimeManagerId() const override;

    void Initialize() override;

    bool RequestStop(_In_ bool Force) override;

    void Stop() override;

    void RegisterPlan9ConnectionTarget(_In_ HANDLE userToken) override;

    const WSLDistributionInformation* DistributionInformation() const noexcept override;

    wil::unique_socket CreateLinuxProcess(_In_ LPCSTR Path, _In_ LPCSTR* Arguments);

private:
    void MountDrvfs(bool Admin) const;

    void ReadOOBEResult(wil::unique_socket&& Socket, wsl::windows::service::DistributionRegistration&& registration);

    std::recursive_mutex m_lock;
    wil::unique_handle m_userToken;
    bool m_initialized = false;
    bool m_adminMountNamespaceCreated = false;
    bool m_nonAdminMountNamespaceCreated = false;
    ULONG m_featureFlags{};
    GUID m_instanceId{};
    GUID m_runtimeId{};
    LXSS_DISTRO_CONFIGURATION m_configuration{};
    ULONG m_clientId{};
    ULONG m_defaultUid{};
    std::function<LX_INIT_DRVFS_MOUNT(HANDLE)> m_initializeDrvFs;
    std::shared_ptr<WslCorePort> m_initChannel;
    std::shared_ptr<ConsoleManager> m_consoleManager;
    ULONG64 m_ntClientLifetimeId{};
    wsl::windows::common::redirector::ConnectionTargetManager m_redirectorConnectionTargets;
    ULONG m_plan9Port{LX_INIT_UTILITY_VM_INVALID_PORT};
    std::shared_ptr<WslCoreInstance> m_systemDistro;
    WSLDistributionInformation m_distributionInfo{};
    DWORD m_socketTimeout{};
    std::thread m_oobeThread;
    wil::unique_event m_destroyingEvent{wil::EventOptions::ManualReset};
    wil::unique_event m_oobeCompleteEvent;
};
