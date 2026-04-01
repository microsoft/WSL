/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerExecTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC container exec command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EContainerExecTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerExecTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    TEST_METHOD(WSLCE2E_Container_Exec_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container exec --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_MissingContainerId)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container exec");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'container-id'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_MissingCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec {}", WslcContainerName));
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'command'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_ContainerNotFound)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container exec {} echo hello", WslcContainerName));
        result.Verify({.ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_SimpleCommand)
    {
        WSL2_TEST_ONLY();

        // Run a container in background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Execute a command
        result = RunWslc(std::format(L"container exec {} echo hello", WslcContainerName));
        result.Verify({.Stdout = L"hello\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_Detach)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Execute with detach flag
        result = RunWslc(std::format(L"container exec -d {} sh -c \"sleep 2 && echo done\"", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Should return immediately without waiting
        // (Verification would require checking that process is still running)
    }

    TEST_METHOD(WSLCE2E_Container_Exec_InteractiveTTY)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -itd --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto& expectedPrompt = VT::InspectAndBuildContainerPrompt(WslcContainerName);

        auto session = RunWslcInteractive(std::format(L"container exec -it {} /bin/bash", WslcContainerName));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        session.ExpectStdout(expectedPrompt);

        session.WriteLine("echo hello");
        session.ExpectCommandEcho("echo hello");
        session.ExpectStdout("hello\r\n");
        session.ExpectStdout(expectedPrompt);

        session.ExitAndVerifyNoErrors();
        auto exitCode = session.Wait();
        VERIFY_ARE_EQUAL(0, exitCode);
    }

    TEST_METHOD(WSLCE2E_Container_Exec_InteractiveNoTTY)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -id --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto session = RunWslcInteractive(std::format(L"container exec -i {} cat", WslcContainerName));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        // Write test data to stdin
        session.WriteLine("test line 1");
        session.WriteLine("test line 2");

        // Close stdin to signal EOF to cat
        session.CloseStdin();

        // Wait for cat to exit with code 0
        auto exitCode = session.Wait(10000);
        VERIFY_ARE_EQUAL(0, exitCode, L"Cat should exit with code 0 after receiving EOF");
        session.VerifyNoErrors();
    }

    TEST_METHOD(WSLCE2E_Container_Exec_VerboseOption)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec --verbose {} echo hello", WslcContainerName));
        result.Verify({.Stdout = L"hello\n", .Stderr = L"", .ExitCode = 0});
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
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
        return L"Executes a command in a running container.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container exec [<options>] <container-id> <command> [<arguments>...]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n"
                 << L"  container-id    Container ID\r\n"
                 << L"  command         The command to run\r\n"
                 << L"  arguments       Arguments to pass to container's init process\r\n"
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -d,--detach       Run container in detached mode\r\n"
                << L"  -e,--env          Key=Value pairs for environment variables\r\n"
                << L"  --env-file        File containing key=value pairs of env variables\r\n"
                << L"  -i,--interactive  Attach to stdin and keep it open\r\n"
                << L"  -t,--tty          Open a TTY with the container process.\r\n"
                << L"  -h,--help         Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
