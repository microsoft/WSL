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

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EContainerRestartTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerRestartTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(WslcContainerName2);
        EnsureImageIsDeleted(DebianImage);
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
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_RunningContainer)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        VerifyContainerIsListed(containerId, L"running");

        result = RunWslc(std::format(L"container restart {} -t 0", containerId));
        result.Verify({.Stdout = std::format(L"{}\r\n", containerId), .Stderr = L"", .ExitCode = 0});

        // Should be running again after restart.
        VerifyContainerIsListed(containerId, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_StoppedContainer)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        result = RunWslc(std::format(L"container stop {} -t 0", containerId));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VerifyContainerIsListed(containerId, L"exited");

        // Restart of a stopped container should just start it.
        result = RunWslc(std::format(L"container restart {} -t 0", containerId));
        result.Verify({.Stdout = std::format(L"{}\r\n", containerId), .Stderr = L"", .ExitCode = 0});
        VerifyContainerIsListed(containerId, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_ByName)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        VerifyContainerIsListed(containerId, L"running");

        result = RunWslc(std::format(L"container restart {} -t 0", WslcContainerName));
        result.Verify({.Stdout = std::format(L"{}\r\n", WslcContainerName), .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(containerId, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_NotFound)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container restart {} -t 0", WslcContainerName));
        result.Verify(
            {.Stderr = std::format(L"Container '{}' not found.\r\nError code: WSLC_E_CONTAINER_NOT_FOUND\r\n", WslcContainerName),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_TargetedContainerOnly)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto firstContainerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(firstContainerId.empty());

        result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName2, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto secondContainerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(secondContainerId.empty());

        VerifyContainerIsListed(firstContainerId, L"running");
        VerifyContainerIsListed(secondContainerId, L"running");

        // Restart only the first container.
        result = RunWslc(std::format(L"container restart {} -t 0", firstContainerId));
        result.Verify({.Stdout = std::format(L"{}\r\n", firstContainerId), .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(firstContainerId, L"running");
        VerifyContainerIsListed(secondContainerId, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_AutoRemoveRefused)
    {
        // --rm containers cannot be restarted (Stop would delete them).
        auto result =
            RunWslc(std::format(L"container run -d --rm --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        VerifyContainerIsListed(containerId, L"running");

        result = RunWslc(std::format(L"container restart {} -t 0", containerId));
        const auto expectedStderr =
            std::format(L"Container '{}' was created with --rm and cannot be restarted.\r\nError code: ERROR_INVALID_STATE\r\n", containerId);
        result.Verify({.Stderr = expectedStderr, .ExitCode = 1});

        // Container should still be running after the refusal.
        VerifyContainerIsListed(containerId, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_InvalidSignal)
    {
        auto result = RunWslc(std::format(L"container run --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container restart {} -s 0 -t 0", WslcContainerName));
        result.Verify({.Stderr = L"Invalid signal value: 0 is out of valid range (1-31).\r\n", .ExitCode = 1});

        result = RunWslc(std::format(L"container restart {} -s 32 -t 0", WslcContainerName));
        result.Verify({.Stderr = L"Invalid signal value: 32 is out of valid range (1-31).\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Restart_InvalidTimeout)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        result = RunWslc(std::format(L"container restart {} -t abc", containerId));
        result.Verify({.Stderr = L"Invalid time argument value: abc\r\n", .ExitCode = 1});

        // Container is unaffected by the parse failure.
        VerifyContainerIsListed(containerId, L"running");
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const std::wstring WslcContainerName2 = L"wslc-test-container-2";
    const TestImage& DebianImage = DebianTestImage();

    std::wstring GetHelpMessage() const
    {
        std::wstringstream output;
        output << GetWslcHeader()        //
               << GetDescription()       //
               << GetUsage()             //
               << GetAvailableCommands() //
               << GetAvailableOptions();
        return output.str();
    }

    std::wstring GetDescription() const
    {
        return Localization::WSLCCLI_ContainerRestartLongDesc() + L"\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container restart [<options>] [<container-id>]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" << L"  container-id    Container ID\r\n" << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -s,--signal     Signal to send\r\n"
                << L"  -t,--time       Time in seconds to wait before executing (default 5)\r\n"
                << L"  -?,--help       Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
