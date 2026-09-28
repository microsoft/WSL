/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerRestartTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"
#include "TestImageRegistry.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EContainerRestartTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerRestartTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        TestImageRegistry::Instance().EnsureLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(WslcContainerName2);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(WslcContainerName2);
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_HelpCommand)
    {
        auto result = RunWslc(L"container restart --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_RunningContainer)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        VerifyContainerIsListed(containerId, L"running");
        const auto startedAt = InspectContainer(WslcContainerName).State.StartedAt;

        result = RunWslc(std::format(L"container restart {} -t 0", containerId));
        result.Verify({.Stdout = std::format(L"{}\r\n", containerId), .Stderr = L"", .ExitCode = 0});

        // The container is running again, but from a new init process.
        VerifyContainerIsListed(containerId, L"running");
        VERIFY_ARE_NOT_EQUAL(startedAt, InspectContainer(WslcContainerName).State.StartedAt);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_ByName)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        VerifyContainerIsListed(containerId, L"running");

        // Restart by container name
        result = RunWslc(std::format(L"container restart {} -t 0", WslcContainerName));
        result.Verify({.Stdout = std::format(L"{}\r\n", WslcContainerName), .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(containerId, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_StoppedContainer)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        result = RunWslc(std::format(L"container stop {} -t 0", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VerifyContainerIsListed(containerId, L"exited");

        // Restarting a stopped container starts it
        result = RunWslc(std::format(L"container restart {} -t 0", containerId));
        result.Verify({.Stdout = std::format(L"{}\r\n", containerId), .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(containerId, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_MultipleContainers)
    {
        // Run first container in background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto firstContainerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(firstContainerId.empty());

        // Run second container in background
        result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto secondContainerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(secondContainerId.empty());

        result = RunWslc(std::format(L"container restart {} {} -t 0", firstContainerId, secondContainerId));
        result.Verify({.Stdout = std::format(L"{}\r\n{}\r\n", firstContainerId, secondContainerId), .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(firstContainerId, L"running");
        VerifyContainerIsListed(secondContainerId, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_ContinuesAfterFailure)
    {
        // Run first container in background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto firstContainerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(firstContainerId.empty());

        // Run second container in background
        result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto secondContainerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(secondContainerId.empty());

        // A container that cannot be restarted is reported without skipping the ones after it
        result = RunWslc(std::format(L"container restart {} {} {} -t 0", firstContainerId, InvalidContainerName, secondContainerId));
        result.Verify(
            {.Stdout = std::format(L"{}\r\n{}\r\n", firstContainerId, secondContainerId),
             .Stderr = FormatErrorMessage(
                 std::format(L"Container '{}' not found.", InvalidContainerName), L"WSLC_E_CONTAINER_NOT_FOUND"),
             .ExitCode = 1});

        VerifyContainerIsListed(firstContainerId, L"running");
        VerifyContainerIsListed(secondContainerId, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_EachFailureIsReported)
    {
        VerifyContainerIsNotListed(InvalidContainerName);
        VerifyContainerIsNotListed(InvalidContainerName2);

        auto result = RunWslc(std::format(L"container restart {} {} -t 0", InvalidContainerName, InvalidContainerName2));
        result.Verify(
            {.Stdout = L"",
             .Stderr = FormatErrorMessage(
                           std::format(L"Container '{}' not found.", InvalidContainerName), L"WSLC_E_CONTAINER_NOT_FOUND") +
                       FormatErrorMessage(
                           std::format(L"Container '{}' not found.", InvalidContainerName2), L"WSLC_E_CONTAINER_NOT_FOUND"),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_NotFound)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container restart {} -t 0", WslcContainerName));
        result.Verify(
            {.Stderr =
                 FormatErrorMessage(std::format(L"Container '{}' not found.", WslcContainerName), L"WSLC_E_CONTAINER_NOT_FOUND"),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_InvalidSignalName)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        VerifyContainerIsListed(containerId, L"running");

        result = RunWslc(std::format(L"container restart {} -s SIGINVALID -t 0", containerId));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            L"Invalid signal value: SIGINVALID is not a recognized signal name or number (Example: SIGKILL, kill, or 9)."));

        // Verify container is still running after failed restart request
        VerifyContainerIsListed(containerId, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_InvalidTimeout)
    {
        // Run a container in the background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        VerifyContainerIsListed(containerId, L"running");

        result = RunWslc(std::format(L"container restart {} -t abc", containerId));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(
            result.StderrContainsSubstring(wsl::shared::Localization::WSLCCLI_InvalidIntegerArgumentError(L"timeout", L"abc")));

        // Verify container is still running after failed restart request
        VerifyContainerIsListed(containerId, L"running");
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const std::wstring WslcContainerName2 = L"wslc-test-container-2";
    const std::wstring InvalidContainerName = L"wslc-nonexistent-container-for-restart";
    const std::wstring InvalidContainerName2 = L"wslc-nonexistent-container-for-restart-2";
    const TestImage& DebianImage = DebianTestImage();
};
} // namespace WSLCE2ETests
