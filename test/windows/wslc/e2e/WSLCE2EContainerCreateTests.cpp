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
        EnsureImageIsLoaded(AlpineImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(AlpineImage);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        VolumeTestFile1 = wsl::windows::common::filesystem::GetTempFilename();
        VolumeTestFile2 = wsl::windows::common::filesystem::GetTempFilename();
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    TEST_METHOD_CLEANUP(TestMethodCleanup)
    {
        DeleteFileW(VolumeTestFile1.c_str());
        DeleteFileW(VolumeTestFile2.c_str());
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

    TEST_METHOD(WSLCE2E_Container_Create_Volume_WriteFromHostReadFromContainer)
    {
        WSL2_TEST_ONLY();

        // Write to a temp file that we will mount as a volume to the container
        const std::wstring tempFile = VolumeTestFile1.wstring();
        std::wofstream out(tempFile);
        out << L"WSLC Volume Test";
        out.close();

        auto hostDirectory = VolumeTestFile1.parent_path();
        auto fileName = VolumeTestFile1.filename().wstring();

        auto result = RunWslc(std::format(
            L"container run --name {} --volume \"{}:/data:ro\" {} cat /data/{}",
            WslcContainerName,
            hostDirectory.wstring(),
            AlpineImage.NameAndTag(),
            fileName));
        result.Verify({.Stdout = L"WSLC Volume Test", .Stderr = L"", .ExitCode = S_OK});
    }

    TEST_METHOD(WSLCE2E_Container_Create_Volume_WriteFromContainerReadFromHost_ReadWritePermissionByDefault)
    {
        WSL2_TEST_ONLY();

        auto hostDirectory = VolumeTestFile1.parent_path();
        auto fileName = VolumeTestFile1.filename().wstring();
        auto result = RunWslc(std::format(
            L"container run --name {} --volume \"{}:/data\" {} sh -c \"echo -n 'WSLC Volume Test' > /data/{}\"",
            WslcContainerName,
            hostDirectory.wstring(),
            AlpineImage.NameAndTag(),
            fileName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = S_OK});

        // Read all file content
        std::wifstream in(VolumeTestFile1);
        std::wstringstream buffer;
        buffer << in.rdbuf();
        VERIFY_ARE_EQUAL(L"WSLC Volume Test", buffer.str());
    }

    TEST_METHOD(WSLCE2E_Container_Create_Volume_WriteFromContainerReadFromHost_ReadWritePermission)
    {
        WSL2_TEST_ONLY();

        auto hostDirectory = VolumeTestFile1.parent_path();
        auto fileName = VolumeTestFile1.filename().wstring();
        auto result = RunWslc(std::format(
            L"container run --name {} --volume \"{}:/data:rw\" {} sh -c \"echo -n 'WSLC Volume Test' > /data/{}\"",
            WslcContainerName,
            hostDirectory.wstring(),
            AlpineImage.NameAndTag(),
            fileName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = S_OK});

        // Read all file content
        std::wifstream in(VolumeTestFile1);
        std::wstringstream buffer;
        buffer << in.rdbuf();
        VERIFY_ARE_EQUAL(L"WSLC Volume Test", buffer.str());
    }

    TEST_METHOD(WSLCE2E_Container_Create_Volume_WriteFromContainerReadFromHost_ReadOnlyPermission_Fail)
    {
        WSL2_TEST_ONLY();

        auto hostDirectory = VolumeTestFile1.parent_path();
        auto fileName = VolumeTestFile1.filename().wstring();
        auto result = RunWslc(std::format(
            L"container run --name {} --volume \"{}:/data:ro\" {} sh -c \"echo -n 'WSLC Volume Test' > /data/{}\"",
            WslcContainerName,
            hostDirectory.wstring(),
            AlpineImage.NameAndTag(),
            fileName));
        auto errorMessage = std::format(L"sh: can't create /data/{}: Read-only file system\n", fileName);
        result.Verify({.Stdout = L"", .Stderr = errorMessage, .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Create_Volume_Multiple_WriteFromContainerReadFromHost_ReadWritePermission)
    {
        WSL2_TEST_ONLY();

        // Mount multiple volumes to the container
        auto hostDirectory1 = VolumeTestFile1.parent_path();
        auto fileName1 = VolumeTestFile1.filename().wstring();
        auto hostDirectory2 = VolumeTestFile2.parent_path();
        auto fileName2 = VolumeTestFile2.filename().wstring();
        auto result = RunWslc(std::format(
            L"container run --name {} --volume \"{}:/data1:rw\" --volume \"{}:/data2:rw\" {} sh -c \"echo -n 'Test1' > "
            L"/data1/{} && "
            L"echo -n 'Test2' > /data2/{}\"",
            WslcContainerName,
            hostDirectory1.wstring(),
            hostDirectory2.wstring(),
            AlpineImage.NameAndTag(),
            fileName1,
            fileName2));

        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Read all file content for both files
        std::wifstream in1(VolumeTestFile1);
        std::wstringstream buffer1;
        buffer1 << in1.rdbuf();
        VERIFY_ARE_EQUAL(L"Test1", buffer1.str());

        std::wifstream in2(VolumeTestFile2);
        std::wstringstream buffer2;
        buffer2 << in2.rdbuf();
        VERIFY_ARE_EQUAL(L"Test2", buffer2.str());
    }

    TEST_METHOD(WSLCE2E_Container_Create_Volume_Invalid)
    {
        WSL2_TEST_ONLY();

        {
            auto result = RunWslc(std::format(L"container run --volume :/containerPath {}", AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"The parameter is incorrect. \r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
        }

        {
            auto result = RunWslc(std::format(L"container run --volume C:\\hostPath::ro {}", AlpineImage.NameAndTag()));
            result.Verify(
                {.Stderr = L"invalid mount config for type \"bind\": field Target must not be empty\r\nError code: E_FAIL\r\n", .ExitCode = 1});
        }

        {
            auto result = RunWslc(std::format(L"container run --volume :/containerPath:ro {}", AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"The parameter is incorrect. \r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
        }

        {
            auto result = RunWslc(std::format(L"container run --volume \"\" {}", AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: ''. Expected format: <host path>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
        }

        {
            auto result = RunWslc(std::format(L"container run --volume C:\\hostPath: {}", AlpineImage.NameAndTag()));
            result.Verify(
                {.Stderr = L"invalid mount config for type \"bind\": field Target must not be empty\r\nError code: E_FAIL\r\n", .ExitCode = 1});
        }

        {
            auto result = RunWslc(std::format(L"container run --volume C:\\hostPath:ro {}", AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"The parameter is incorrect. \r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
        }

        {
            auto result = RunWslc(std::format(L"container run --volume :ro {}", AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: ':ro'. Expected format: <host path>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
        }

        {
            auto result = RunWslc(std::format(L"container run --volume C:\\hostPath::rw {}", AlpineImage.NameAndTag()));
            result.Verify(
                {.Stderr = L"invalid mount config for type \"bind\": field Target must not be empty\r\nError code: E_FAIL\r\n", .ExitCode = 1});
        }

        {
            auto result =
                RunWslc(std::format(L"container run --volume C:\\hostPath:/containerPath:invalid_mode {}", AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"The system cannot find the path specified. \r\nError code: ERROR_PATH_NOT_FOUND\r\n", .ExitCode = 1});
        }

        {
            auto result =
                RunWslc(std::format(L"container run --volume C:\\hostPath:/containerPath:ro:extra {}", AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"The system cannot find the path specified. \r\nError code: ERROR_PATH_NOT_FOUND\r\n", .ExitCode = 1});
        }

        {
            auto result = RunWslc(std::format(L"container run --volume C:\\hostPath:/containerPath: {}", AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"The system cannot find the path specified. \r\nError code: ERROR_PATH_NOT_FOUND\r\n", .ExitCode = 1});
        }
    }

    TEST_METHOD(WSLCE2E_Container_Create_Volume_NotSupported)
    {
        // Commands tested in this method are not currently supported in WSLC,
        // so we just verify that they fail with the expected error message.
        // https://github.com/microsoft/WSL/issues/14432
        WSL2_TEST_ONLY();

        {
            auto result = RunWslc(std::format(L"container run --volume \"C:\\hostPath\" {}", AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"The parameter is incorrect. \r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
        }

        {
            auto result = RunWslc(std::format(L"container run --volume \"C:/hostPath\" {}", AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"The parameter is incorrect. \r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
        }

        {
            auto result = RunWslc(std::format(L"container run --volume \":\" {}", AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"The parameter is incorrect. \r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
        }

        {
            auto result = RunWslc(std::format(L"container run --volume \"::\" {}", AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"The parameter is incorrect. \r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
        }

        {
            auto result = RunWslc(std::format(L"container run --volume \"e2e_test\" {}", AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'e2e_test'. Expected format: <host path>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
        }
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

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& AlpineImage = AlpineTestImage();
    const TestImage& InvalidImage = InvalidTestImage();
    std::filesystem::path VolumeTestFile1;
    std::filesystem::path VolumeTestFile2;

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