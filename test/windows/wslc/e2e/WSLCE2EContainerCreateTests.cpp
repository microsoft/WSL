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
#include <fstream>
#include <wil/network.h>
#include <wil/resource.h>

namespace WSLCE2ETests {
using namespace wsl::shared;

using namespace WEX::Logging;

class WSLCE2EContainerCreateTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerCreateTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(AlpineImage);
        EnsureImageIsLoaded(DebianImage);

        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName.c_str(), HostEnvVariableValue.c_str()));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName2.c_str(), HostEnvVariableValue2.c_str()));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(MissingHostEnvVariableName.c_str(), nullptr));
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(AlpineImage);
        EnsureImageIsDeleted(DebianImage);

        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName.c_str(), nullptr));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName2.c_str(), nullptr));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(MissingHostEnvVariableName.c_str(), nullptr));
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnvTestFile1 = wsl::windows::common::filesystem::GetTempFilename();
        EnvTestFile2 = wsl::windows::common::filesystem::GetTempFilename();
        VolumeTestFile1 = wsl::windows::common::filesystem::GetTempFilename();
        VolumeTestFile2 = wsl::windows::common::filesystem::GetTempFilename();
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    TEST_METHOD_CLEANUP(TestMethodCleanup)
    {
        DeleteFileW(EnvTestFile1.c_str());
        DeleteFileW(EnvTestFile2.c_str());
        DeleteFileW(VolumeTestFile1.c_str());
        DeleteFileW(VolumeTestFile2.c_str());
        return true;
    }

    TEST_METHOD(WSLCE2E_Container_Create_HelpCommand)
    {
        WSL2_TEST_ONLY();
        auto result = RunWslc(L"container create --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
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
                      << L"manifest for " << InvalidImage.NameAndTag()
                      << L" not found: manifest unknown: manifest tagged by \"latest\" is not found\r\n"
                      << L"Error code: WSLC_E_IMAGE_NOT_FOUND\r\n";
        result.Verify({.Stderr = expectedError.str(), .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Create_Valid)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        // Create the container with a valid image
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
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
        result.Verify({.Stderr = L"", .ExitCode = 0});
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
        result.Verify({.Stdout = L"WSLC Volume Test", .Stderr = L"", .ExitCode = 0});
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
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Read all file content
        auto content = ReadFileContent(VolumeTestFile1.wstring());
        VERIFY_ARE_EQUAL(L"WSLC Volume Test", content);
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
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

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
            auto result =
                RunWslc(std::format(L"container run --name {} --volume :/containerPath {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: ':/containerPath'. Host path cannot be empty. Expected format: <host path>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(
                std::format(L"container run --name {} --volume C:\\hostPath::ro {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Unspecified error \r\nError code: E_FAIL\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(
                std::format(L"container run --name {} --volume :/containerPath:ro {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: ':/containerPath:ro'. Host path cannot be empty. Expected format: <host path>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(L"container run --name {} --volume \"\" {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: ''. Expected format: <host path>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container run --name {} --volume C:\\hostPath: {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Unspecified error \r\nError code: E_FAIL\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container run --name {} --volume C:\\hostPath:ro {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'C:\\hostPath:ro'. Container path must be an absolute path (starting with '/'). Expected format: <host path>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(L"container run --name {} --volume :ro {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: ':ro'. Expected format: <host path>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(
                std::format(L"container run --name {} --volume C:\\hostPath::rw {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Unspecified error \r\nError code: E_FAIL\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(
                L"container run --name {} --volume C:\\hostPath:/containerPath:invalid_mode {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'C:\\hostPath:/containerPath:invalid_mode'. Container path must be an absolute path (starting with '/'). Expected format: <host path>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(
                L"container run --name {} --volume C:\\hostPath:/containerPath:ro:extra {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'C:\\hostPath:/containerPath:ro:extra'. Container path must be an absolute path (starting with '/'). Expected format: <host path>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(
                L"container run --name {} --volume C:\\hostPath:/containerPath: {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Unspecified error \r\nError code: E_FAIL\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }
    }

    TEST_METHOD(WSLCE2E_Container_Create_Volume_NotSupported)
    {
        // Commands tested in this method are not currently supported in WSLC,
        // so we just verify that they fail with the expected error message.
        // https://github.com/microsoft/WSL/issues/14432
        WSL2_TEST_ONLY();

        {
            auto result = RunWslc(
                std::format(L"container run --name {} --volume \"C:\\hostPath\" {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'C:\\hostPath'. Container path must be an absolute path (starting with '/'). Expected format: <host path>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container run --name {} --volume \"C:/hostPath\" {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"The parameter is incorrect. \r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(L"container run --name {} --volume \":\" {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: ':'. Host path cannot be empty. Expected format: <host path>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container run --name {} --volume \"::\" {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Unspecified error \r\nError code: E_FAIL\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container run --name {} --volume \"e2e_test\" {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'e2e_test'. Expected format: <host path>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }
    }

    TEST_METHOD(WSLCE2E_Container_Create_Remove)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container create --rm --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Start the container.
        result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify with retry timeout of 1 minute.
        VerifyContainerIsNotListed(WslcContainerName, std::chrono::seconds(2), std::chrono::minutes(1));
    }

    TEST_METHOD(WSLCE2E_Container_CreateStartAttach_TTY)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        const auto& prompt = ">";
        auto result = RunWslc(std::format(
            L"container create -it -e PS1={} --name {} {} bash --norc", prompt, WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = result.GetStdoutOneLine();

        const auto& expectedPrompt = VT::BuildContainerPrompt(prompt);

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

    TEST_METHOD(WSLCE2E_Container_CreateStartAttach_NoTTY)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);
        auto result = RunWslc(std::format(L"container create -i --name {} {} cat", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = result.GetStdoutOneLine();

        // Start with attach
        auto session = RunWslcInteractive(std::format(L"container start --attach {}", containerId));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        session.WriteLine("test line 1");
        session.ExpectStdout("test line 1\n");
        session.WriteLine("test line 2");
        session.ExpectStdout("test line 2\n");

        // Close stdin to signal EOF to cat
        session.CloseStdin();

        // Wait for cat to exit with code 0
        auto exitCode = session.Wait(10000);
        VERIFY_ARE_EQUAL(0, exitCode, L"Cat should exit with code 0 after receiving EOF");
        session.VerifyNoErrors();
    }

    TEST_METHOD(WSLCE2E_Container_CreateStartAttach_ShortRunningInitProcess)
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

    TEST_METHOD(WSLCE2E_Container_Create_UserOption_UidRoot)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(
            std::format(L"container create --name {} -u 0 {} sh -c \"id -u; id -g\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Create_UserOption_NameGroupRoot)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(
            L"container create --name {} -u root:root {} sh -c \"id -un; id -u; id -g\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"root\n0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Create_UserOption_UnknownUser_Fails)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(
            std::format(L"container create --name {} -u user_does_not_exist {} id -u", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify(
            {.Stderr = L"unable to find user user_does_not_exist: no matching entries in passwd file\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

private:
    // Test container name
    const std::wstring WslcContainerName = L"wslc-test-container";

    // Test environment variables
    const std::wstring HostEnvVariableName = L"WSLC_TEST_HOST_ENV";
    const std::wstring HostEnvVariableName2 = L"WSLC_TEST_HOST_ENV2";
    const std::wstring HostEnvVariableValue = L"wslc-host-env-value";
    const std::wstring HostEnvVariableValue2 = L"wslc-host-env-value2";
    const std::wstring MissingHostEnvVariableName = L"WSLC_TEST_MISSING_HOST_ENV";

    // Test environment variable files
    std::filesystem::path EnvTestFile1;
    std::filesystem::path EnvTestFile2;

    // Test images
    const TestImage& AlpineImage = AlpineTestImage();
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& InvalidImage = InvalidTestImage();

    // Test volume files
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
        return Localization::WSLCCLI_ContainerCreateLongDesc() + L"\r\n\r\n";
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
                << L"  --entrypoint      Specifies the container init process executable\r\n"
                << L"  -e,--env          Key=Value pairs for environment variables\r\n"
                << L"  --env-file        File containing key=value pairs of env variables\r\n"
                << L"  -i,--interactive  Attach to stdin and keep it open\r\n"
                << L"  --name            Name of the container\r\n"
                << L"  -p,--publish      Publish a port from a container to host\r\n"
                << L"  --rm              Remove the container after it stops\r\n"
                << L"  --session         Specify the session to use\r\n"
                << L"  -t,--tty          Open a TTY with the container process.\r\n"
                << L"  -u,--user         User ID for the process (name|uid|uid:gid)\r\n"
                << L"  -v,--volume       Bind mount a volume to the container\r\n"
                << L"  -h,--help         Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests