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
        auto cleanup = wil::scope_exit([&]() { RemoveDockerContainerNoThrow(corruptContainerId); });

        // Terminate the default session so the next wslc command recreates it and runs recovery.
        EnsureSessionIsTerminated();

        // Run the CLI: recovery of the corrupt container fails, but because the recovery runs
        // outside the user's current command, the warning is not printed on stderr.
        auto result = RunWslc(L"container list");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value());

        const auto recoveryWarning =
            wsl::shared::Localization::MessageWslcFailedToRecoverContainer(string::MultiByteToWide(corruptContainerId));

        // Assert that capture actually worked before searching it, matching the other e2e tests:
        // RunWslc always populates Stderr, so a missing value means capture broke and the search
        // below would pass without proving anything.
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(recoveryWarning) == std::wstring::npos);
    }
};

} // namespace WSLCE2ETests
