// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "../../../src/windows/wslcsession/WSLCStartup.h"

namespace WSLCLocalhostTests {

using wsl::windows::service::wslc::CheckGuestHostsFileWriteResult;
using wsl::windows::service::wslc::WaitForDockerStartup;

class WSLCLocalhostUnitTests
{
    WSLC_TEST_CLASS(WSLCLocalhostUnitTests)

    TEST_METHOD(Initialization_HostsWriteFailurePreservesGuestError)
    {
        for (const int error : {EACCES, EROFS})
        {
            HRESULT result{};
            {
                wsl::windows::common::COMServiceExecutionContext context;
                result = wil::ResultFromException([&]() { CheckGuestHostsFileWriteResult(error); });
            }
            VERIFY_ARE_EQUAL(E_FAIL, result);
            ValidateCOMErrorMessage(wsl::shared::Localization::MessageWslcLocalhostWriteFailed("/etc/hosts", error));
        }
    }

    TEST_METHOD(Startup_ReadySucceedsWithoutCheckingExitCode)
    {
        wil::unique_event ready{wil::EventOptions::ManualReset};
        wil::unique_event exited{wil::EventOptions::ManualReset};
        wil::unique_event terminating{wil::EventOptions::ManualReset};
        ready.SetEvent();
        unsigned exitCodeReads = 0;
        WaitForDockerStartup(ready.get(), exited.get(), terminating.get(), 0, [&]() {
            ++exitCodeReads;
            return 42;
        });
        VERIFY_ARE_EQUAL(0u, exitCodeReads);
    }

    TEST_METHOD(Startup_TimeoutReportsReadinessFailureWithoutDnsAttribution)
    {
        wil::unique_event ready{wil::EventOptions::ManualReset};
        wil::unique_event exited{wil::EventOptions::ManualReset};
        wil::unique_event terminating{wil::EventOptions::ManualReset};
        HRESULT result{};
        {
            wsl::windows::common::COMServiceExecutionContext context;
            result = wil::ResultFromException(
                [&]() { WaitForDockerStartup(ready.get(), exited.get(), terminating.get(), 0, []() { return 42; }); });
        }
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_TIMEOUT), result);
        ValidateCOMErrorMessage(wsl::shared::Localization::MessageWslcDockerStartupTimedOut(0));
    }

    TEST_METHOD(Startup_ProcessExitTakesPrecedenceOverReadiness)
    {
        wil::unique_event ready{wil::EventOptions::ManualReset};
        wil::unique_event exited{wil::EventOptions::ManualReset};
        wil::unique_event terminating{wil::EventOptions::ManualReset};
        ready.SetEvent();
        exited.SetEvent();
        HRESULT result{};
        {
            wsl::windows::common::COMServiceExecutionContext context;
            result = wil::ResultFromException(
                [&]() { WaitForDockerStartup(ready.get(), exited.get(), terminating.get(), 0, []() { return 42; }); });
        }
        VERIFY_ARE_EQUAL(E_FAIL, result);
        ValidateCOMErrorMessage(wsl::shared::Localization::MessageWslcDockerExitedBeforeReady(42));
    }

    TEST_METHOD(Startup_CancellationTakesPrecedenceOverReadiness)
    {
        wil::unique_event ready{wil::EventOptions::ManualReset};
        wil::unique_event exited{wil::EventOptions::ManualReset};
        wil::unique_event terminating{wil::EventOptions::ManualReset};
        ready.SetEvent();
        terminating.SetEvent();
        VERIFY_THROWS_SPECIFIC(
            WaitForDockerStartup(ready.get(), exited.get(), terminating.get(), 0, []() { return 42; }),
            wil::ResultException,
            [](const wil::ResultException& error) { return error.GetErrorCode() == E_ABORT; });
    }
};

} // namespace WSLCLocalhostTests
