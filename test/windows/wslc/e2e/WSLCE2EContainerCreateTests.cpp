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
#include "WSLCExecutorHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EContainerCreateTests
{
    WSL_TEST_CLASS(WSLCE2EContainerCreateTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(WSLCE2E_Container_Create_HelpCommand)
    {
        auto result = RunWslc(L"container create --help");
        result.VerifyNoErrors(GetOutput());
    }

    TEST_METHOD(WSLCE2E_Container_Create_MissingImage)
    {
        auto result = RunWslc(L"container create --name " + WslcContainerName);
        result.Verify({.Stdout = GetOutput(), .Stderr = L"Required argument not provided: 'image'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Create_InvalidImage)
    {
        auto result = RunWslc(L"container create --name " + WslcContainerName + L" " + WslcInvalidImageNameAndTag);
        std::wstringstream expectedError;
        expectedError << L"Image '" << WslcInvalidImageNameAndTag << L"' not found, pulling\r\n"
                      << L"pull access denied for library/"
                      << WslcInvalidImageName << L", repository does not exist or may require 'docker login': denied: requested access to the resource is denied\r\n"
                      << L"Error code: WSLA_E_IMAGE_NOT_FOUND\r\n";
        result.Verify({.Stderr = expectedError.str(), .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Create_Valid)
    {
        // Ensure the container does not already exist
        EnsureContainerDoesNotExist(WslcContainerName);

        // Create the container with a valid image
        auto result = RunWslc(L"container create --name " + WslcContainerName + L" " + WslcUbuntuImageName);
        result.VerifyNoErrors();
        std::wstring containerId = result.GetStdoutOneLine();

        // Verify the container is listed with the correct status
        VerifyContainerIsListed(containerId, L"created");

        // Delete the container
        result = RunWslc(L"container delete " + WslcContainerName + L" --force");
        result.VerifyNoErrors();

        // Verify the container is deleted
        VerifyContainerIsNotListed(containerId);
    }

    TEST_METHOD(WSLCE2E_Container_Create_DuplicateContainerName)
    {
        // Ensure the container does not already exist
        EnsureContainerDoesNotExist(WslcContainerName);

        // Create the container with a valid image
        auto result = RunWslc(L"container create --name " + WslcContainerName + L" " + WslcUbuntuImageName);
        result.VerifyNoErrors();
        auto containerId = result.GetStdoutOneLine();

        // Attempt to create another container with the same name
        result = RunWslc(L"container create --name " + WslcContainerName + L" " + WslcUbuntuImageName);
        result.Dump();
        result.Verify(
            {.Stderr = L"Conflict. The container name \"/" + WslcContainerName + L"\" is already in use by container \"" +
                       containerId + L"\". You have to remove (or rename) that container to be able to reuse that name.\r\nError code: ERROR_ALREADY_EXISTS\r\n",
             .ExitCode = 1});

        // Clean up by deleting the container
        result = RunWslc(L"container delete " + WslcContainerName + L" --force");
        result.VerifyNoErrors();
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const std::wstring WslcInvalidImageName = L"mcr.microsoft.com/invalid-image";
    const std::wstring WslcInvalidImageNameAndTag = WslcInvalidImageName + L":latest";
    const std::wstring WslcUbuntuImageName = L"ubuntu:latest";

    std::wstring GetOutput() const
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
                << L"  -rm,--remove      Remove the container after it stops\r\n"
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