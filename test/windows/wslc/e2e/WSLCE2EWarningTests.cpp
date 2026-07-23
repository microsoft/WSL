/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EWarningTests.cpp

Abstract:

    End-to-end tests validating how warnings emitted by the WSLC COM service are
    surfaced (or intentionally suppressed) on the wslc.exe CLI's stderr via the
    IWarningCallback integration.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include <WSLCProcessLauncher.h>

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EWarningTests
{
    WSLC_TEST_CLASS(WSLCE2EWarningTests)

    const TestImage AlpineImage = AlpineTestImage();
    WSADATA m_wsaData{};

    TEST_CLASS_SETUP(ClassSetup)
    {
        THROW_IF_WIN32_ERROR(WSAStartup(MAKEWORD(2, 2), &m_wsaData));
        EnsureImageIsLoaded(AlpineImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureImageIsDeleted(AlpineImage);
        WSACleanup();
        return true;
    }

    static std::string RunDockerInSession(IWSLCSession& session, std::vector<std::string>&& args)
    {
        wsl::windows::common::WSLCProcessLauncher launcher("/usr/bin/docker", args);
        auto result = launcher.Launch(session).WaitAndCaptureOutput();
        VERIFY_ARE_EQUAL(0, result.Code);

        // Trim trailing whitespace from the captured stdout (fd 1).
        auto output = result.Output[1];
        output.erase(output.find_last_not_of(" \n\r") + 1);
        return output;
    }

    // Best-effort removal of a container from docker.
    static void RemoveContainerNoThrow(const std::string& containerId)
    try
    {
        wil::com_ptr<IWSLCSessionManager> sessionManager;
        THROW_IF_FAILED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());

        wil::com_ptr<IWSLCSession> session;
        THROW_IF_FAILED(sessionManager->OpenSessionByName(nullptr, &session));
        wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

        wsl::windows::common::WSLCProcessLauncher launcher("/usr/bin/docker", {"/usr/bin/docker", "rm", "-f", containerId});
        launcher.Launch(*session).WaitAndCaptureOutput();
    }
    CATCH_LOG()

    // Injects a container with corrupt WSLC metadata into the default session's storage,
    // then verifies that running the wslc.exe CLI does not surface the COM service's recovery
    // warning on stderr: recovery runs outside the user's current command, so it is logged
    // (and written to the event log) rather than streamed back via IWarningCallback.
    WSLC_TEST_METHOD(WSLCE2E_Warning_ContainerRecoveryNotPrintedOnStderr)
    {
        std::string corruptContainerId;

        // Inject a container whose WSLC metadata label is not valid JSON. RecoverExistingContainers
        // will fail to parse it the next time the session is created.
        {
            auto session = OpenDefaultElevatedSession();
            corruptContainerId = RunDockerInSession(
                *session,
                {"/usr/bin/docker",
                 "create",
                 "--label",
                 "wslc.container.metadata=INVALID_JSON",
                 string::WideToMultiByte(AlpineImage.NameAndTag())});
            VERIFY_IS_FALSE(corruptContainerId.empty());
        }

        // cleanup: remove the corrupt container
        auto cleanup = wil::scope_exit([&]() { RemoveContainerNoThrow(corruptContainerId); });

        // Terminate the default session so the next wslc command recreates it and runs recovery.
        EnsureSessionIsTerminated();

        // Run the CLI: recovery of the corrupt container fails, but because the recovery runs
        // outside the user's current command, the warning is not printed on stderr.
        auto result = RunWslc(L"container list");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value());

        const auto recoveryWarning =
            wsl::shared::Localization::MessageWslcFailedToRecoverContainer(string::MultiByteToWide(corruptContainerId));
        if (result.Stderr.has_value())
        {
            VERIFY_IS_TRUE(result.Stderr.value().find(recoveryWarning) == std::wstring::npos);
        }
    }
};

} // namespace WSLCE2ETests
