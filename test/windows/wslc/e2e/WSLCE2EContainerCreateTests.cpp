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

class WSLCE2EContainerCreateTests
{
    WSL_TEST_CLASS(WSLCE2EContainerCreateTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);

        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName.c_str(), HostEnvVariableValue.c_str()));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(MissingHostEnvVariableName.c_str(), nullptr));
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);

        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName.c_str(), nullptr));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(MissingHostEnvVariableName.c_str(), nullptr));
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

        // Create the container with a valid image
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

    TEST_METHOD(WSLCE2E_Container_Run_EnvOption)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {}=A {} env", WslcContainerName, HostEnvVariableName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        bool foundEnv = false;
        for (const auto& line : outputLines)
        {
            if (line == std::format(L"{}=A", HostEnvVariableName))
            {
                foundEnv = true;
                break;
            }
        }

        VERIFY_IS_TRUE(foundEnv);
    }

    TEST_METHOD(WSLCE2E_Container_Run_EnvOption_MultipleValues)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {}=A -e {}=B {} env",
            WslcContainerName,
            HostEnvVariableName,
            HostEnvVariableName2,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        bool foundFirstEnv = false;
        bool foundSecondEnv = false;
        for (const auto& line : result.GetStdoutLines())
        {
            if (line == std::format(L"{}=A", HostEnvVariableName))
            {
                foundFirstEnv = true;
            }
            else if (line == std::format(L"{}=B", HostEnvVariableName2))
            {
                foundSecondEnv = true;
            }
        }

        VERIFY_IS_TRUE(foundFirstEnv);
        VERIFY_IS_TRUE(foundSecondEnv);
    }

    TEST_METHOD(WSLCE2E_Container_Run_EnvOption_KeyOnly_UsesHostValue)
    {
        // https://github.com/microsoft/WSL/issues/14474
        SKIP_TEST_NOT_IMPL();

        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {} {} env", WslcContainerName, HostEnvVariableName, DebianImage.NameAndTag()));
        result.Dump();
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto expectedLine = std::format(L"{}={}", HostEnvVariableName, HostEnvVariableValue);
        bool foundEnv = false;
        for (const auto& line : result.GetStdoutLines())
        {
            if (line == expectedLine)
            {
                foundEnv = true;
                break;
            }
        }

        VERIFY_IS_TRUE(foundEnv);
    }

    TEST_METHOD(WSLCE2E_Container_Run_EnvOption_KeyOnly_HostVariableMissing)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {} {} env", WslcContainerName, MissingHostEnvVariableName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto prefix = std::format(L"{}=", MissingHostEnvVariableName);
        bool foundEnv = false;
        for (const auto& line : result.GetStdoutLines())
        {
            if (line.rfind(prefix, 0) == 0)
            {
                foundEnv = true;
                break;
            }
        }

        VERIFY_IS_FALSE(foundEnv);
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const std::wstring HostEnvVariableName = L"WSLC_TEST_HOST_ENV";
    const std::wstring HostEnvVariableName2 = L"WSLC_TEST_HOST_ENV2";
    const std::wstring HostEnvVariableValue = L"wslc-host-env-value";
    const std::wstring HostEnvVariableValue2 = L"wslc-host-env-value2";
    const std::wstring MissingHostEnvVariableName = L"WSLC_TEST_MISSING_HOST_ENV";
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