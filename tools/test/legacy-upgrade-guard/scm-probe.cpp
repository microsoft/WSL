// Copyright (C) Microsoft Corporation. All rights reserved.
// Run explicitly in a disposable elevated test VM, never as an automatic test.
#include "ServiceUpgradeGuard.h"
#include <cstdio>

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc != 3 || !std::wstring(argv[1]).starts_with(L"WslGuardProbe-"))
        {
            std::puts("Usage: scm-probe WslGuardProbe-<unique-name> normal|crash");
            return 1;
        }

        const std::wstring mode(argv[2]);
        if (mode != L"normal" && mode != L"crash")
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
