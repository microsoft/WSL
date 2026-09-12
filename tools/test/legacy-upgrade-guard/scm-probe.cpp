// Copyright (C) Microsoft Corporation. All rights reserved.
// Run explicitly in a disposable elevated test VM, never as an automatic test.
#include "ServiceUpgradeGuard.h"
#include <cstdio>
#include <msi.h>
#include <string>

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if ((argc != 3 && argc != 4) || !std::wstring(argv[1]).starts_with(L"WslGuardProbe-"))
        {
            std::puts("Usage: scm-probe WslGuardProbe-<unique-name> normal|crash|recover|hold|install [fixture.msi]");
            return 1;
        }

        const std::wstring mode(argv[2]);
        if (mode == L"install" && argc == 4)
        {
            UINT result = 0;
            {
                const ServiceUpgradeGuard guard(argv[1]);
                MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
                const auto logPath = std::wstring(argv[3]) + L".log";
                MsiEnableLogW(INSTALLLOGMODE_VERBOSE | INSTALLLOGMODE_ERROR | INSTALLLOGMODE_ACTIONSTART, logPath.c_str(), 0);
                result = MsiInstallProductW(argv[3], L"REBOOT=ReallySuppress FAIL_ROLLBACK=1");
            }
            std::printf("MSI result: %u\n", result);
            return static_cast<int>(result);
        }
        if (mode == L"recover")
        {
            THROW_IF_FAILED(ServiceUpgradeGuard::Recover(argv[1]));
            std::puts("PASS: persisted recovery completed");
            return 0;
        }
        if (mode != L"normal" && mode != L"crash" && mode != L"hold")
        {
            return 1;
        }

        {
            const ServiceUpgradeGuard guard(argv[1]);
            const wil::unique_schandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
            THROW_LAST_ERROR_IF(!manager);
            const wil::unique_schandle service{OpenServiceW(manager.get(), argv[1], SERVICE_START)};
            THROW_LAST_ERROR_IF(!service);
            const auto started = StartServiceW(service.get(), 0, nullptr);
            THROW_HR_IF(E_UNEXPECTED, started || GetLastError() != ERROR_SERVICE_DISABLED);
            std::puts("PASS: SCM rejected activation with ERROR_SERVICE_DISABLED");
            std::fflush(stdout);
            if (mode == L"hold")
            {
                Sleep(4000);
            }

            if (mode == L"crash")
            {
                // Simulate loss of the updater without C++ stack unwinding.
                TerminateProcess(GetCurrentProcess(), 99);
                return 1;
            }
        }

        std::puts("PASS: guard returned normally");
        return 0;
    }
    catch (...)
    {
        std::puts("FAIL: SCM guard probe");
        return 1;
    }
}
