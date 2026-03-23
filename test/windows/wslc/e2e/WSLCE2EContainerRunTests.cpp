/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerRunTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EContainerRunTests
{
    WSLA_TEST_CLASS(WSLCE2EContainerRunTests)

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

    TEST_METHOD(WSLCE2E_Container_Run_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container run --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Run_Container_With_Command)
    {
        WSL2_TEST_ONLY();

        VerifyContainerIsNotListed(WslcContainerName);

        auto command = L"echo echo_from_container";
        auto result = RunWslc(std::format(L"container run --name {} {} {}", WslcContainerName, DebianImage.NameAndTag(), command));
        result.Verify({.Stdout = L"echo_from_container\n", .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(WslcContainerName, L"exited");
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
        return L"Runs a container. By default, the container is started in the background; use --detach to run in the "
               L"foreground.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container run [<options>] <image> [<command>] [<arguments>...]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n"
                 << L"  image             Image name\r\n"
                 << L"  command           The command to run\r\n"
                 << L"  arguments         Arguments to pass to container's init process\r\n"
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  --cidfile         Write the container ID to the provided path.\r\n"
                << L"  -d,--detach       Run container in detached mode\r\n"
                << L"  --dns             IP address of the DNS nameserver in resolv.conf\r\n"
                << L"  --dns-domain      Set the default DNS Domain\r\n"
                << L"  --dns-option      Set DNS options\r\n"
                << L"  --dns-search      Set DNS search domains\r\n"
                << L"  --entrypoint      Specifies the container init process executable\r\n"
                << L"  -e,--env          Key=Value pairs for environment variables\r\n"
                << L"  --env-file        File containing key=value pairs of env variables\r\n"
                << L"  -i,--interactive  Attach to stdin and keep it open\r\n"
                << L"  --name            Name of the container\r\n"
                << L"  --no-dns          No configuration of DNS in the container\r\n"
                << L"  --progress        Progress type (format: none|ansi) (default: ansi)\r\n"
                << L"  -p,--publish      Publish a port from a container to host\r\n"
                << L"  --pull            Image pull policy (always|missing|never) (default:never)\r\n"
                << L"  --rm              Remove the container after it stops\r\n"
                << L"  --scheme          Use this scheme for registry connection\r\n"
                << L"  --session         Specify the session to use\r\n"
                << L"  --tmpfs           Mount tmpfs to the container at the given path\r\n"
                << L"  -t,--tty          Open a TTY with the container process.\r\n"
                << L"  -u,--user         User ID for the process (name|uid|uid:gid)\r\n"
                << L"  --volume          Bind mount a volume to the container\r\n"
                << L"  --virtualization  Expose virtualization capabilities to the container\r\n"
                << L"  -h,--help         Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests