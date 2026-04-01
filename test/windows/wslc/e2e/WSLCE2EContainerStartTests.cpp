/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerStartTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC container start command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EContainerStartTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerStartTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        EnsureImageIsLoaded(AlpineImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);
        EnsureImageIsDeleted(AlpineImage);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    TEST_METHOD(WSLCE2E_Container_Start_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container start --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Start_ContainerNotFound)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stderr = L"Element not found. \r\nError code: ERROR_NOT_FOUND\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Start_MissingContainerId)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container start");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'container-id'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Start_Basic)
    {
        WSL2_TEST_ONLY();

        // Create a container
        auto result = RunWslc(std::format(L"container create --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = result.GetStdoutOneLine();

        // Start it
        result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Stop it
        result = RunWslc(std::format(L"container stop {} -t 0", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Start_AttachTTY)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);
        auto result = RunWslc(std::format(L"container create -it --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = result.GetStdoutOneLine();

        const auto& expectedPrompt = VT::InspectAndBuildContainerPrompt(WslcContainerName);

        auto session = RunWslcInteractive(std::format(L"container start --attach {}", containerId));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        session.ExpectStdout(expectedPrompt);

        session.WriteLine("echo hello");
        session.ExpectCommandEcho("echo hello");
        session.ExpectStdout("hello\r\n");
        session.ExpectStdout(expectedPrompt);

        session.WriteLine("whoami");
        session.ExpectCommandEcho("whoami");
        session.ExpectStdout("root\r\n");
        session.ExpectStdout(expectedPrompt);

        session.ExitAndVerifyNoErrors();
        auto exitCode = session.Wait();
        VERIFY_ARE_EQUAL(0, exitCode);
    }

    TEST_METHOD(WSLCE2E_Container_Start_AttachNoTTY)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);
        auto result = RunWslc(std::format(L"container create -i --name {} {} cat", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = result.GetStdoutOneLine();

        // Start with attach
        auto session = RunWslcInteractive(std::format(L"container start --attach {}", containerId));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        // Write test data to stdin
        session.WriteLine("test line 1");
        session.WriteLine("test line 2");

        // Stdin relay is confirmed working. Stdout verification is skipped due to a known
        // limitation where we are not getting stdout data correctly from non-TTY process.
        // BUG: Stdin does not support overlapped IO. Can verify output once this is fixed.

        // Close stdin to signal EOF to cat
        session.CloseStdin();

        // Wait for cat to exit with code 0
        auto exitCode = session.Wait(10000);
        VERIFY_ARE_EQUAL(0, exitCode, L"Cat should exit with code 0 after receiving EOF");
        session.VerifyNoErrors();
    }

    TEST_METHOD(WSLCE2E_Container_Start_AttachShortRunningInitProcess)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        constexpr auto ExpectedExitCode = 37;

        auto result = RunWslc(std::format(
            L"container create --name {} {} sh -c \"echo lifecycle works; exit {}\"", WslcContainerName, AlpineImage.NameAndTag(), ExpectedExitCode));

        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"lifecycle works\n", .Stderr = L"", .ExitCode = ExpectedExitCode});
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& AlpineImage = AlpineTestImage();

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
        return L"Starts a container. Provides options to attach to the container's stdout and stderr streams and could be interactive to keep stdin open.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container start [<options>] <container-id>\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n" //
                 << L"  container-id      Container ID\r\n"         //
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -a,--attach       Attach to stdout/stderr of the container\r\n"
                << L"  -i,--interactive  Attach to stdin and keep it open\r\n"
                << L"  --session         Specify the session to use\r\n"
                << L"  -h,--help         Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
