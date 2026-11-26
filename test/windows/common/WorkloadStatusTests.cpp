// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "InstallGuard.h"
#include "WslInstall.h"
#include "wslc.h"
#include "wslpolicies.h"

extern std::wstring g_testDistroPath;

namespace {

wil::unique_handle OpenWslServiceProcess(DWORD access)
{
    const wil::unique_schandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    THROW_LAST_ERROR_IF(!manager);
    const wil::unique_schandle service{OpenServiceW(manager.get(), L"WslService", SERVICE_QUERY_STATUS)};
    THROW_LAST_ERROR_IF(!service);

    const auto [state, pid] = GetServiceState(service.get());
    VERIFY_ARE_EQUAL(static_cast<DWORD>(SERVICE_RUNNING), state);
    wil::unique_handle process{OpenProcess(access, FALSE, pid)};
    THROW_LAST_ERROR_IF(!process);
    return process;
}

wil::unique_handle GetServiceImpersonationToken()
{
    const auto privilege = wsl::windows::common::security::AcquirePrivileges({SE_DEBUG_NAME});
    const auto process = OpenWslServiceProcess(PROCESS_QUERY_LIMITED_INFORMATION);
    wil::unique_handle token;
    VERIFY_WIN32_BOOL_SUCCEEDED(OpenProcessToken(process.get(), TOKEN_QUERY | TOKEN_DUPLICATE, token.put()));

    wil::unique_handle impersonationToken;
    VERIFY_WIN32_BOOL_SUCCEEDED(DuplicateTokenEx(
        token.get(), TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, impersonationToken.put()));
    VERIFY_IS_TRUE(wsl::windows::common::security::IsTokenLocalSystem(impersonationToken.get()));

    auto [serviceSid, serviceSidBuffer] = wsl::windows::common::security::GetAccountSid(L"NT SERVICE\\WslService");
    BOOL isService{};
    VERIFY_WIN32_BOOL_SUCCEEDED(CheckTokenMembership(impersonationToken.get(), serviceSid, &isService));
    VERIFY_IS_TRUE(isService);
    return impersonationToken;
}

} // namespace

// Privileged callers must see workloads owned by any user.
// Run these lifecycle tests without unrelated WSL or WSLC workloads.
class MachineWideWorkloadStatusTests
{
    WSL_TEST_CLASS(MachineWideWorkloadStatusTests)

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        VERIFY_ARE_EQUAL(TRUE, LxsstuInitialize(FALSE));
        m_initialized = true;
        WslShutdown();
        VerifyServiceBecomesIdle();
        return true;
    }

    TEST_METHOD_CLEANUP(TestMethodCleanup)
    {
        if (m_initialized)
        {
            auto uninitialize = wil::scope_exit([&]() {
                LxsstuUninitialize(FALSE);
                m_initialized = false;
            });
            WslShutdown();
            VerifyServiceBecomesIdle();
        }

        return true;
    }

    void ValidateRunningWslInstances(bool Wsl1Running, bool Wsl2Running)
    {
        std::optional<RegistryKeyChange<DWORD>> wsl1Policy;
        if (Wsl1Running)
        {
            if (!WslInstall::IsOptionalComponentInstalled(WslInstall::c_optionalFeatureNameWsl))
            {
                LogSkipped("This case requires the WSL1 optional component");
                return;
            }

            wsl1Policy.emplace(HKEY_LOCAL_MACHINE, wsl::windows::policies::c_registryKey, wsl::windows::policies::c_allowWSL1, 1);
        }

        std::optional<WslConfigChange> configuration;
        if (Wsl2Running)
        {
            // Disable idle VM shutdown so the VM-only busy check below does not depend on timing.
            configuration.emplace(LxssGenerateTestConfig({.vmIdleTimeout = -1}));
        }

        const wsl::windows::common::filesystem::StagingDirectory storage(std::filesystem::temp_directory_path());
        std::vector<std::wstring> distroNames;
        // Shut down before deleting the temporary distro storage, including on assertion failures.
        auto shutdownOnExit = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { WslShutdown(); });
        auto unregister = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            for (const auto& distro : distroNames)
            {
                VERIFY_ARE_EQUAL(0u, LxsstuLaunchWsl(std::format(L"--unregister {}", distro)));
            }
        });

        std::vector<std::unique_ptr<WslKeepAlive>> instances;
        const auto startDistro = [&](int version) {
            const auto& name =
                distroNames.emplace_back(std::format(L"workload-status-{}-{}", version, storage.Path().filename().wstring()));
            const auto path = storage.Path() / std::to_wstring(version);
            VERIFY_ARE_EQUAL(
                0u, LxsstuLaunchWsl(std::format(L"--import {} \"{}\" \"{}\" --version {}", name, path.wstring(), g_testDistroPath, version)));
            instances.emplace_back(std::make_unique<WslKeepAlive>(nullptr, name));
        };

        if (Wsl1Running)
        {
            startDistro(1);
        }

        if (Wsl2Running)
        {
            startDistro(2);
        }

        VERIFY_ARE_EQUAL(Wsl1Running || Wsl2Running, wsl::windows::common::install::IsServiceBusy());

        instances.clear();
        // Dropping the keep-alives only ends their commands; stop the distributions before checking VM-only activity.
        for (const auto& distro : distroNames)
        {
            VERIFY_ARE_EQUAL(0u, LxsstuLaunchWsl(std::format(L"--terminate {}", distro)));
        }

        // The WSL2 VM remains a workload even after its distributions stop.
        VERIFY_ARE_EQUAL(Wsl2Running, wsl::windows::common::install::IsServiceBusy());
        WslShutdown();
        VERIFY_IS_FALSE(wsl::windows::common::install::IsServiceBusy());
    }

    TEST_METHOD(DetectsNoRunningWslInstances)
    {
        ValidateRunningWslInstances(false, false);
    }

    TEST_METHOD(DetectsOnlyRunningWsl1)
    {
        ValidateRunningWslInstances(true, false);
    }

    WSL2_TEST_METHOD(DetectsOnlyRunningWsl2)
    {
        ValidateRunningWslInstances(false, true);
    }

    WSL2_TEST_METHOD(DetectsRunningWsl1AndWsl2)
    {
        ValidateRunningWslInstances(true, true);
    }

    WSL2_TEST_METHOD(ReportsIdleAfterFailedVmStartup)
    {
        RegistryKeyChange<DWORD> kernelPolicy(
            HKEY_LOCAL_MACHINE, wsl::windows::policies::c_registryKey, wsl::windows::policies::c_allowCustomKernelUserSetting, 1);
        const wsl::windows::common::filesystem::StagingDirectory storage(std::filesystem::temp_directory_path());
        const auto kernel = (storage.Path() / L"missing-kernel").wstring();
        WslConfigChange configuration(LxssGenerateTestConfig({.kernel = kernel}));

        VERIFY_ARE_NOT_EQUAL(0u, LxsstuLaunchWsl(L"--exec true"));
        VERIFY_IS_FALSE(wsl::windows::common::install::IsServiceBusy());
    }

    TEST_METHOD(StatusQueryDoesNotStartStoppedService)
    {
        auto restart = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { RestartWslService(); });
        StopWslService();

        VERIFY_IS_FALSE(wsl::windows::common::install::IsServiceBusy());
        VERIFY_IS_FALSE(wsl::windows::common::helpers::IsServiceRunning(L"WslService"));
    }

    TEST_METHOD(UnprivilegedCallersCannotOpenBusyEvent)
    {
        const auto token = GetNonElevatedToken(TokenImpersonation);
        const auto impersonate = wil::impersonate_token(token.get());

        const wil::unique_handle event{OpenEventW(SYNCHRONIZE, FALSE, wsl::windows::common::install::c_serviceBusyEventName)};
        const auto error = GetLastError();
        VERIFY_IS_NULL(event.get());
        VERIFY_ARE_EQUAL(static_cast<DWORD>(ERROR_ACCESS_DENIED), error);
    }

    TEST_METHOD(IdleQueryDoesNotChangeEvent)
    {
        const wil::unique_handle event{OpenEventW(SYNCHRONIZE, FALSE, wsl::windows::common::install::c_serviceBusyEventName)};
        VERIFY_IS_NOT_NULL(event.get());

        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(event.get(), 0));
        VERIFY_IS_FALSE(wsl::windows::common::install::IsServiceBusy());
        VERIFY_IS_FALSE(wsl::windows::common::install::IsServiceBusy());
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(event.get(), 0));
    }

    TEST_METHOD(AdministratorCannotModifyBusyEvent)
    {
        const wil::unique_handle event{OpenEventW(EVENT_MODIFY_STATE, FALSE, wsl::windows::common::install::c_serviceBusyEventName)};
        const auto error = GetLastError();
        VERIFY_IS_NULL(event.get());
        VERIFY_ARE_EQUAL(static_cast<DWORD>(ERROR_ACCESS_DENIED), error);
    }

    TEST_METHOD(RejectsEventOwnedByAnotherAccount)
    {
        auto restart = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() { RestartWslService(); });
        StopWslService();

        wil::unique_event event;
        bool alreadyExists{};
        event.create(wil::EventOptions::ManualReset, wsl::windows::common::install::c_serviceBusyEventName, nullptr, &alreadyExists);
        VERIFY_IS_FALSE(alreadyExists);
        VERIFY_THROWS_SPECIFIC(wsl::windows::common::install::IsServiceBusy(), wil::ResultException, [](const wil::ResultException& e) {
            return e.GetErrorCode() == E_ACCESSDENIED;
        });
    }

    TEST_METHOD(RetainedSignaledEventIsResetAfterRestart)
    {
        wil::unique_event event;
        {
            const auto token = GetServiceImpersonationToken();
            const auto impersonate = wil::impersonate_token(token.get());
            event.open(wsl::windows::common::install::c_serviceBusyEventName, SYNCHRONIZE | EVENT_MODIFY_STATE);
        }

        auto reset = event.ResetEvent_scope_exit();
        event.SetEvent();
        VERIFY_IS_TRUE(wsl::windows::common::install::IsServiceBusy());

        RestartWslService();
        VERIFY_IS_FALSE(wsl::windows::common::install::IsServiceBusy());
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(event.get(), 0));
    }

    TEST_METHOD(RetainedEventTracksWorkloadsAfterRestart)
    {
        const wil::unique_handle event{OpenEventW(SYNCHRONIZE, FALSE, wsl::windows::common::install::c_serviceBusyEventName)};
        VERIFY_IS_NOT_NULL(event.get());

        RestartWslService();
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            {
                WslKeepAlive workload;
                VERIFY_IS_TRUE(wsl::windows::common::install::IsServiceBusy());
                VERIFY_IS_TRUE(wsl::windows::common::install::IsServiceBusy());
                VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(event.get(), 0));
            }

            WslShutdown();
            VerifyServiceBecomesIdle();
            VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(event.get(), 0));
        }
    }

    TAEF_BEGIN_TEST_METHOD_PROPERTIES_IN_CLASS_SCOPE(TerminatedWslcSessionDoesNotKeepServiceBusy)
    TEST_METHOD_PROPERTY(L"Ignore[not(@Version=2)]", L"true")
    TEST_METHOD_PROPERTY(L"TestCategory", L"WSLC")
    TAEF_END_TEST_METHOD_PROPERTIES_IN_CLASS_SCOPE()
    TEST_METHOD(TerminatedWslcSessionDoesNotKeepServiceBusy)
    {
        wil::com_ptr<IWSLCSessionManager> manager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(manager.get());

        WSLCSessionSettings settings{};
        settings.DisplayName = L"wsl-workload-status-termination-test";
        settings.CpuCount = 1;
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;

        wil::com_ptr<IWSLCSession> session;
        VERIFY_SUCCEEDED(manager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session));
        wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { VERIFY_SUCCEEDED(session->Terminate()); });
        VERIFY_IS_TRUE(wsl::windows::common::install::IsServiceBusy());

        wil::unique_handle hostProcess;
        VERIFY_SUCCEEDED(session->GetProcessHandle(hostProcess.put()));
        VERIFY_SUCCEEDED(session->Terminate());
        // Retaining the terminated session keeps its host alive, but is not a workload.
        VerifyServiceBecomesIdle();
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(hostProcess.get(), 0));
    }

    TAEF_BEGIN_TEST_METHOD_PROPERTIES_IN_CLASS_SCOPE(DetectsWslcSessionHostExit)
    TEST_METHOD_PROPERTY(L"Ignore[not(@Version=2)]", L"true")
    TEST_METHOD_PROPERTY(L"TestCategory", L"WSLC")
    TAEF_END_TEST_METHOD_PROPERTIES_IN_CLASS_SCOPE()
    TEST_METHOD(DetectsWslcSessionHostExit)
    {
        wil::com_ptr<IWSLCSessionManager> manager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(manager.get());

        WSLCSessionSettings settings{};
        settings.DisplayName = L"wsl-workload-status-host-exit-test";
        settings.CpuCount = 1;
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;

        wil::com_ptr<IWSLCSession> session;
        VERIFY_SUCCEEDED(manager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session));
        wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { VERIFY_SUCCEEDED(session->Terminate()); });
        VERIFY_IS_TRUE(wsl::windows::common::install::IsServiceBusy());

        wil::unique_handle hostProcess;
        wil::unique_handle terminatedEvent;
        VERIFY_SUCCEEDED(session->GetProcessHandle(hostProcess.put()));
        VERIFY_SUCCEEDED(session->GetTerminationEvent(terminatedEvent.put()));

        settings.DisplayName = L"wsl-workload-status-surviving-entry-test";
        wil::com_ptr<IWSLCSession> survivingSession;
        VERIFY_SUCCEEDED(manager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &survivingSession));
        wsl::windows::common::security::ConfigureForCOMImpersonation(survivingSession.get());
        auto survivingCleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { VERIFY_SUCCEEDED(survivingSession->Terminate()); });
        ULONG survivingId{};
        wil::unique_handle survivingHost;
        VERIFY_SUCCEEDED(survivingSession->GetId(&survivingId));
        VERIFY_SUCCEEDED(survivingSession->GetProcessHandle(survivingHost.put()));

        VERIFY_WIN32_BOOL_SUCCEEDED(TerminateProcess(hostProcess.get(), 1));
        cleanup.release();
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(hostProcess.get(), 10000));
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_TIMEOUT), WaitForSingleObject(terminatedEvent.get(), 0));

        // Opening the surviving session removes the exited entry and compacts the session vector.
        wil::com_ptr<IWSLCSession> reopenedSession;
        VERIFY_SUCCEEDED(manager->OpenSession(survivingId, reopenedSession.put()));
        VERIFY_IS_TRUE(wsl::windows::common::install::IsServiceBusy());

        VERIFY_WIN32_BOOL_SUCCEEDED(TerminateProcess(survivingHost.get(), 1));
        survivingCleanup.release();
        VERIFY_ARE_EQUAL(static_cast<DWORD>(WAIT_OBJECT_0), WaitForSingleObject(survivingHost.get(), 10000));
        VerifyServiceBecomesIdle();
    }

    // Automatic updates query as SYSTEM, while the workload belongs to the non-SYSTEM test user.
    TEST_METHOD(SystemCallerSeesAnotherUsersWorkload)
    {
        wil::unique_handle userToken;
        VERIFY_WIN32_BOOL_SUCCEEDED(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, userToken.put()));
        wil::unique_handle userImpersonationToken;
        VERIFY_WIN32_BOOL_SUCCEEDED(DuplicateTokenEx(
            userToken.get(), TOKEN_QUERY, nullptr, SecurityImpersonation, TokenImpersonation, userImpersonationToken.put()));
        VERIFY_IS_FALSE(wsl::windows::common::security::IsTokenLocalSystem(userImpersonationToken.get()));
        WslKeepAlive workload;

        const auto impersonationToken = GetServiceImpersonationToken();
        const auto impersonate = wil::impersonate_token(impersonationToken.get());
        VERIFY_IS_TRUE(wsl::windows::common::install::IsServiceBusy());
    }

    bool m_initialized = false;
};
