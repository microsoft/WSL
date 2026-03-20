/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerCreateTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

using namespace WEX::Logging;

class WSLCE2EContainerCreateTests
{
    WSL_TEST_CLASS(WSLCE2EContainerCreateTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    TEST_METHOD(WSLCE2E_Container_Create_HelpCommand)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"container create --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = S_OK});
    }

    TEST_METHOD(WSLCE2E_Container_Create_MissingImage)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"container create --name " + WslcContainerName);
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'image'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Create_InvalidImage)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"container create --name " + WslcContainerName + L" " + InvalidImage.NameAndTag());
        std::wstringstream expectedError;
        expectedError << L"Image '" << InvalidImage.NameAndTag() << L"' not found, pulling\r\n"
                      << L"pull access denied for library/"
                      << InvalidImage.Name << L", repository does not exist or may require 'docker login': denied: requested access to the resource is denied\r\n"
                      << L"Error code: WSLA_E_IMAGE_NOT_FOUND\r\n";
        result.Verify({.Stderr = expectedError.str(), .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Create_Valid)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        // Create the container with a valid image
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        std::wstring containerId = result.GetStdoutOneLine();

        // Verify the container is listed with the correct status
        VerifyContainerIsListed(containerId, L"created");
    }

    TEST_METHOD(WSLCE2E_Container_Create_DuplicateContainerName)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        // Create the container with a valid image
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();

        // Attempt to create another container with the same name
        result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr = std::format(L"Conflict. The container name \"/{}\" is already in use by container \"{}\". You have to remove (or rename) that container to be able to reuse that name.\r\nError code: ERROR_ALREADY_EXISTS\r\n", WslcContainerName, containerId),
             .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Create_Remove)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container create --rm --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Start the container.
        result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify with retry timeout of 1 minute.
        VerifyContainerIsNotListed(WslcContainerName, std::chrono::seconds(2), std::chrono::minutes(1));
    }

    TEST_METHOD(WSLCE2E_Container_Run_Remove)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        // Run the container with a valid image
        auto result = RunWslc(std::format(L"container run --rm --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Run should be deleted on return so no retry.
        VerifyContainerIsNotListed(WslcContainerName);
    }

    TEST_METHOD(WSLCE2E_Container_RunInteractive_TTY)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto session = RunWslcInteractive(std::format(L"container run -it --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        session.ExpectStdout(VT::CONTAINER_PROMPT);

        session.WriteLine("echo hello");
        session.ExpectStdout(std::format("echo hello\r\n{}\rhello\r\n", VT::B_END));
        session.ExpectStdout(VT::CONTAINER_PROMPT);

        session.WriteLine("whoami");
        session.ExpectStdout(std::format("whoami\r\n{}\rroot\r\n", VT::B_END));
        session.ExpectStdout(VT::CONTAINER_PROMPT);

        auto exitCode = session.ExitAndVerifyNoErrors();
        VERIFY_ARE_EQUAL(0, exitCode);
    }

    TEST_METHOD(WSLCE2E_Container_RunInteractive_NoTTY)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto session = RunWslcInteractive(std::format(L"container run -i --name {} {} cat", WslcContainerName, DebianImage.NameAndTag()));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        // Write test data to stdin
        session.WriteLine("test line 1");
        session.WriteLine("test line 2");

        // Stdin relay is confirmed working. Stdout verification is skipped due to a known
        // limitation where we are not getting stdout data correctly from non-TTY process.
        // Calling session.ReadUntil() or WriteAndVerifyOutput(session, "test", "test")
        // fails due to not receiving any output in the pipe.

        // Close stdin to signal EOF to cat
        session.CloseStdin();

        // Wait for cat to exit with code 0
        auto exitCode = session.Wait(10000);
        VERIFY_ARE_EQUAL(0, exitCode, L"Cat should exit with code 0 after receiving EOF");
        session.VerifyNoErrors();
    }

    TEST_METHOD(WSLCE2E_Container_RunAttach_TTY)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);
        auto result = RunWslc(std::format(L"container run -itd --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();

        auto session = RunWslcInteractive(std::format(L"container attach {}", containerId));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        // The container attach prompt appears twice.
        session.ExpectStdout(VT::CONTAINER_ATTACH_PROMPT);
        session.ExpectStdout(VT::CONTAINER_ATTACH_PROMPT);

        session.WriteLine("echo hello");
        session.ExpectStdout(std::format("echo hello\r\n{}\rhello\r\n", VT::B_END));
        session.ExpectStdout(VT::CONTAINER_PROMPT);

        session.WriteLine("whoami");
        session.ExpectStdout(std::format("whoami\r\n{}\rroot\r\n", VT::B_END));
        session.ExpectStdout(VT::CONTAINER_PROMPT);

        session.ExitAndVerifyNoErrors();
        auto exitCode = session.Wait();
        VERIFY_ARE_EQUAL(0, exitCode);
    }

    TEST_METHOD(WSLCE2E_Container_RunAttach_NoTTY)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);
        auto result = RunWslc(std::format(L"container run -id --name {} {} cat", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();

        auto session = RunWslcInteractive(std::format(L"container attach {}", containerId));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        // Write test data to stdin
        session.WriteLine("test line 1");
        session.WriteLine("test line 2");

        // Stdin relay is confirmed working. Stdout verification is skipped due to a known
        // limitation where we are not getting stdout data correctly from non-TTY process.
        // Calling session.ReadUntil() or WriteAndVerifyOutput(session, "test", "test")
        // fails due to not receiving any output in the pipe.

        // Close stdin to signal EOF to cat
        session.CloseStdin();

        // Wait for cat to exit with code 0
        auto exitCode = session.Wait(10000);
        VERIFY_ARE_EQUAL(0, exitCode, L"Cat should exit with code 0 after receiving EOF");
        session.VerifyNoErrors();
    }

    TEST_METHOD(WSLCE2E_Container_ExecInteractive_TTY)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);
        auto result = RunWslc(std::format(L"container run -itd --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();

        auto session = RunWslcInteractive(std::format(L"container exec -it {} /bin/bash", containerId));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        session.ExpectStdout(VT::CONTAINER_PROMPT);

        session.WriteLine("echo hello");
        session.ExpectStdout(std::format("echo hello\r\n{}\rhello\r\n", VT::B_END));
        session.ExpectStdout(VT::CONTAINER_PROMPT);

        session.WriteLine("whoami");
        session.ExpectStdout(std::format("whoami\r\n{}\rroot\r\n", VT::B_END));
        session.ExpectStdout(VT::CONTAINER_PROMPT);

        session.ExitAndVerifyNoErrors();
        auto exitCode = session.Wait();
        VERIFY_ARE_EQUAL(0, exitCode);
    }

    TEST_METHOD(WSLCE2E_Container_ExecInteractive_NoTTY)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);
        auto result = RunWslc(std::format(L"container run -id --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();

        auto session = RunWslcInteractive(std::format(L"container exec -i {} cat", containerId));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        // Write test data to stdin
        session.WriteLine("test line 1");
        session.WriteLine("test line 2");

        // Stdin relay is confirmed working. Stdout verification is skipped due to a known
        // limitation where we are not getting stdout data correctly from non-TTY process.
        // Calling session.ReadUntil() or WriteAndVerifyOutput(session, "test", "test")
        // fails due to not receiving any output in the pipe.

        // Close stdin to signal EOF to cat
        session.CloseStdin();

        // Wait for cat to exit with code 0
        auto exitCode = session.Wait(10000);
        VERIFY_ARE_EQUAL(0, exitCode, L"Cat should exit with code 0 after receiving EOF");
        session.VerifyNoErrors();
    }

    TEST_METHOD(WSLCE2E_Container_CreateStartAttach_TTY)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);
        auto result = RunWslc(std::format(L"container create -it --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();

        auto session = RunWslcInteractive(std::format(L"container start --attach {}", containerId));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        // The container attach prompt appears twice.
        session.ExpectStdout(VT::CONTAINER_ATTACH_PROMPT);
        session.ExpectStdout(VT::CONTAINER_ATTACH_PROMPT);

        session.WriteLine("echo hello");
        session.ExpectStdout(std::format("echo hello\r\n{}\rhello\r\n", VT::B_END));
        session.ExpectStdout(VT::CONTAINER_PROMPT);

        session.WriteLine("whoami");
        session.ExpectStdout(std::format("whoami\r\n{}\rroot\r\n", VT::B_END));
        session.ExpectStdout(VT::CONTAINER_PROMPT);

        session.ExitAndVerifyNoErrors();
        auto exitCode = session.Wait();
        VERIFY_ARE_EQUAL(0, exitCode);
    }

    TEST_METHOD(WSLCE2E_Container_CreateStartAttach_NoTTY)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);
        auto result = RunWslc(std::format(L"container create -i --name {} {} cat", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();

        // Start with attach
        auto session = RunWslcInteractive(std::format(L"container start --attach {}", containerId));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        // Write test data to stdin
        session.WriteLine("test line 1");
        session.WriteLine("test line 2");

        // Stdin relay is confirmed working. Stdout verification is skipped due to a known
        // limitation where we are not getting stdout data correctly from non-TTY process.
        // Calling session.ReadUntil() or WriteAndVerifyOutput(session, "test", "test")
        // fails due to not receiving any output in the pipe.

        // Close stdin to signal EOF to cat
        session.CloseStdin();

        // Wait for cat to exit with code 0
        auto exitCode = session.Wait(10000);
        VERIFY_ARE_EQUAL(0, exitCode, L"Cat should exit with code 0 after receiving EOF");
        session.VerifyNoErrors();
    }

    TEST_METHOD(WSLCE2E_Session_Shell)
    {
        WSL2_TEST_ONLY();
        {
            // Session shell should attach to the wslc session.
            auto session = RunWslcInteractive(L"session shell");
            VERIFY_IS_TRUE(session.IsRunning(), L"Session should be running");

            session.ExpectStdout(VT::SESSION_PROMPT);

            session.WriteLine("echo hello");
            session.ExpectStdout(std::format("{}echo hello\r\n{}\rhello\r\n", VT::RESET, VT::B_END));
            session.ExpectStdout(VT::SESSION_PROMPT);

            session.WriteLine("whoami");
            session.ExpectStdout(std::format("{}whoami\r\n{}\rroot\r\n", VT::RESET, VT::B_END));
            session.ExpectStdout(VT::SESSION_PROMPT);

            session.ExitAndVerifyNoErrors();
            auto exitCode = session.Wait();
            VERIFY_ARE_EQUAL(0, exitCode);
        }
        {
            // Session shell should attach to the wslc by name also.
            auto session = RunWslcInteractive(L"session shell wsla-cli");
            VERIFY_IS_TRUE(session.IsRunning(), L"Session should be running");

            session.ExpectStdout(VT::SESSION_PROMPT);

            session.WriteLine("echo hello");
            session.ExpectStdout(std::format("{}echo hello\r\n{}\rhello\r\n", VT::RESET, VT::B_END));
            session.ExpectStdout(VT::SESSION_PROMPT);

            session.WriteLine("whoami");
            session.ExpectStdout(std::format("{}whoami\r\n{}\rroot\r\n", VT::RESET, VT::B_END));
            session.ExpectStdout(VT::SESSION_PROMPT);

            session.ExitAndVerifyNoErrors();
            auto exitCode = session.Wait();
            VERIFY_ARE_EQUAL(0, exitCode);
        }
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& InvalidImage = InvalidTestImage();

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
        return L"Creates a container.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container create [<options>] <image> [<command>] [<arguments>...]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n"
                 << L"  image             Image name\r\n"
                 << L"  command           The command to run\r\n"
                 << L"  arguments         Arguments to pass to container's init process\r\n\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n" //
                << L"  --cidfile         Write the container ID to the provided path.\r\n"
                << L"  --dns             IP address of the DNS nameserver in resolv.conf\r\n"
                << L"  --dns-domain      Set the default DNS Domain\r\n"
                << L"  --dns-option      Set DNS options\r\n"
                << L"  --dns-search      Set DNS search domains\r\n"
                << L"  --entrypoint      Specifies the container init process executable\r\n"
                << L"  -e,--env          Key=Value pairs for environment variables\r\n"
                << L"  --env-file        File containing key=value pairs of env variables\r\n"
                << L"  --groupid         Group Id for the process\r\n"
                << L"  -i,--interactive  Attach to stdin and keep it open\r\n"
                << L"  --name            Name of the container\r\n"
                << L"  --no-dns          No configuration of DNS in the container\r\n"
                << L"  --progress        Progress type (format: none|ansi) (default: ansi)\r\n"
                << L"  --rm              Remove the container after it stops\r\n"
                << L"  --scheme          Use this scheme for registry connection\r\n"
                << L"  --session         Specify the session to use\r\n"
                << L"  --tmpfs           Mount tmpfs to the container at the given path\r\n"
                << L"  -t,--tty          Open a TTY with the container process.\r\n"
                << L"  -u,--user         User ID for the process (name|uid|uid:gid)\r\n"
                << L"  --volume          Bind mount a volume to the container\r\n"
                << L"  --virtualization  Expose virtualization capabilities to the container\r\n"
                << L"  -h,--help         Shows help about the selected command\r\n\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests