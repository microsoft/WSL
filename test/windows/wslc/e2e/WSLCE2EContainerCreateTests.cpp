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
        EnsureImageIsLoaded(PythonImage);

        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName.c_str(), HostEnvVariableValue.c_str()));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName2.c_str(), HostEnvVariableValue2.c_str()));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(MissingHostEnvVariableName.c_str(), nullptr));

        // Initialize Winsock for loopback connectivity tests
        WSADATA wsaData{};
        const int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        THROW_HR_IF(HRESULT_FROM_WIN32(result), result != 0);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(AlpineImage);
        EnsureImageIsDeleted(DebianImage);
        EnsureImageIsDeleted(PythonImage);

        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName.c_str(), nullptr));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName2.c_str(), nullptr));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(MissingHostEnvVariableName.c_str(), nullptr));

        // Cleanup Winsock
        WSACleanup();
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_HelpCommand)
    {
        auto result = RunWslc(L"container create --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = S_OK});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_MissingImage)
    {
        auto result = RunWslc(L"container create --name " + WslcContainerName);
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'image'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_InvalidImage)
    {
        auto result = RunWslc(L"container create --name " + WslcContainerName + L" " + InvalidImage.NameAndTag());
        std::wstringstream expectedError;
        expectedError << L"Image '" << InvalidImage.NameAndTag() << L"' not found, pulling\r\n"
                      << L"manifest for " << InvalidImage.NameAndTag()
                      << L" not found: manifest unknown: manifest tagged by \"latest\" is not found\r\n"
                      << L"Error code: WSLC_E_IMAGE_NOT_FOUND\r\n";
        result.Verify({.Stderr = expectedError.str(), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Valid)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Create the container with a valid image
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        std::wstring containerId = result.GetStdoutOneLine();

        // Verify the container is listed with the correct status
        VerifyContainerIsListed(containerId, L"created");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_DuplicateContainerName)
    {
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Volume_WriteFromHostReadFromContainer)
    {
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Volume_WriteFromContainerReadFromHost_ReadWritePermissionByDefault)
    {
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
        auto content = ReadFileContent(VolumeTestFile1.wstring());
        VERIFY_ARE_EQUAL(L"WSLC Volume Test", content);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Volume_WriteFromContainerReadFromHost_ReadWritePermission)
    {
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Volume_WriteFromContainerReadFromHost_ReadOnlyPermission_Fail)
    {
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Volume_Multiple_WriteFromContainerReadFromHost_ReadWritePermission)
    {
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Volume_Invalid)
    {
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Volume_NotSupported)
    {
        // Commands tested in this method are not currently supported in WSLC,
        // so we just verify that they fail with the expected error message.
        // https://github.com/microsoft/WSL/issues/14432
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Remove)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container create --rm --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Start the container.
        result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Verify with retry timeout of 1 minute.
        VerifyContainerIsNotListed(WslcContainerName, std::chrono::seconds(2), std::chrono::minutes(1));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Remove)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Run the container with a valid image
        auto result = RunWslc(std::format(L"container run --rm --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Run should be deleted on return so no retry.
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvOption)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {}=A {} env", WslcContainerName, HostEnvVariableName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}=A", HostEnvVariableName)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvOption_MultipleValues)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {}=A -e {}=B {} env",
            WslcContainerName,
            HostEnvVariableName,
            HostEnvVariableName2,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}=A", HostEnvVariableName)));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}=B", HostEnvVariableName2)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvOption_KeyOnly_UsesHostValue)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {} {} env", WslcContainerName, HostEnvVariableName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}={}", HostEnvVariableName, HostEnvVariableValue)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvOption_KeyOnly_MultipleValues_UsesHostValues)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {} -e {} {} env",
            WslcContainerName,
            HostEnvVariableName,
            HostEnvVariableName2,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}={}", HostEnvVariableName, HostEnvVariableValue)));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}={}", HostEnvVariableName2, HostEnvVariableValue2)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvOption_EmptyValue)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {}= {} env", WslcContainerName, HostEnvVariableName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}=", HostEnvVariableName)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvFile)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        WriteEnvFile(EnvTestFile1, {"WSLC_TEST_ENV_FILE_A=env-file-a", "WSLC_TEST_ENV_FILE_B=env-file-b"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_ENV_FILE_A=env-file-a"));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_ENV_FILE_B=env-file-b"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvOption_MixedWithEnvFile)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        WriteEnvFile(EnvTestFile1, {"WSLC_TEST_ENV_MIX_FILE_A=from-file-a", "WSLC_TEST_ENV_MIX_FILE_B=from-file-b"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e WSLC_TEST_ENV_MIX_CLI=from-cli --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_ENV_MIX_FILE_A=from-file-a"));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_ENV_MIX_FILE_B=from-file-b"));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_ENV_MIX_CLI=from-cli"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvFile_MultipleFiles)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        WriteEnvFile(EnvTestFile1, {"WSLC_TEST_ENV_FILE_MULTI_A=file1-a", "WSLC_TEST_ENV_FILE_MULTI_B=file1-b"});

        WriteEnvFile(EnvTestFile2, {"WSLC_TEST_ENV_FILE_MULTI_C=file2-c", "WSLC_TEST_ENV_FILE_MULTI_D=file2-d"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            EscapePath(EnvTestFile2.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_ENV_FILE_MULTI_A=file1-a"));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_ENV_FILE_MULTI_B=file1-b"));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_ENV_FILE_MULTI_C=file2-c"));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_ENV_FILE_MULTI_D=file2-d"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvFile_MissingFile)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file ENV_FILE_NOT_FOUND {} env", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr = L"Environment file 'ENV_FILE_NOT_FOUND' cannot be opened for reading\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvFile_InvalidContent)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        WriteEnvFile(EnvTestFile1, {"WSLC_TEST_ENV_VALID=ok", "BAD KEY=value"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Environment variable key 'BAD KEY' cannot contain whitespace\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvFile_DuplicateKeys_Precedence)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        WriteEnvFile(EnvTestFile1, {"WSLC_TEST_ENV_DUP=from-file-1"});

        WriteEnvFile(EnvTestFile2, {"WSLC_TEST_ENV_DUP=from-file-2"});

        // Later --env-file should win over earlier --env-file for duplicate keys
        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            EscapePath(EnvTestFile2.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_ENV_DUP=from-file-2"));

        // Explicit -e should win over env-file value for duplicate keys
        result = RunWslc(std::format(
            L"container run --rm --name {} -e WSLC_TEST_ENV_DUP=from-cli --env-file {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            EscapePath(EnvTestFile2.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_ENV_DUP=from-cli"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvFile_ValueContainsEquals)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        WriteEnvFile(EnvTestFile1, {"WSLC_TEST_ENV_EQUALS=value=with=equals"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_ENV_EQUALS=value=with=equals"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvOption)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec -e {}=A {} env", HostEnvVariableName, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}=A", HostEnvVariableName)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvOption_KeyOnly_UsesHostValue)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec -e {} {} env", HostEnvVariableName, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}={}", HostEnvVariableName, HostEnvVariableValue)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvFile)
    {
        WriteEnvFile(EnvTestFile1, {"WSLC_TEST_EXEC_ENV_FILE_A=exec-env-file-a", "WSLC_TEST_EXEC_ENV_FILE_B=exec-env-file-b"});

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec --env-file {} {} env", EscapePath(EnvTestFile1.wstring()), WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_FILE_A=exec-env-file-a"));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_FILE_B=exec-env-file-b"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvOption_MixedWithEnvFile)
    {
        WriteEnvFile(EnvTestFile1, {"WSLC_TEST_EXEC_ENV_MIX_FILE_A=from-file-a", "WSLC_TEST_EXEC_ENV_MIX_FILE_B=from-file-b"});

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(
            L"container exec -e WSLC_TEST_EXEC_ENV_MIX_CLI=from-cli --env-file {} {} env", EscapePath(EnvTestFile1.wstring()), WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_MIX_FILE_A=from-file-a"));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_MIX_FILE_B=from-file-b"));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_MIX_CLI=from-cli"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvFile_MissingFile)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec --env-file ENV_FILE_NOT_FOUND {} env", WslcContainerName));
        result.Verify(
            {.Stderr = L"Environment file 'ENV_FILE_NOT_FOUND' cannot be opened for reading\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_RunInteractive_TTY)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        const auto& prompt = ">";
        auto session = RunWslcInteractive(
            std::format(L"container run -it -e PS1={} --name {} {} bash --norc", prompt, WslcContainerName, DebianImage.NameAndTag()));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        const auto& expectedPrompt = VT::BuildContainerPrompt(prompt);
        session.ExpectStdout(expectedPrompt);

        session.WriteLine("echo hello");
        session.ExpectCommandEcho("echo hello");
        session.ExpectStdout("hello\r\n");
        session.ExpectStdout(expectedPrompt);

        session.WriteLine("whoami");
        session.ExpectCommandEcho("whoami");
        session.ExpectStdout("root\r\n");
        session.ExpectStdout(expectedPrompt);

        auto exitCode = session.ExitAndVerifyNoErrors();
        VERIFY_ARE_EQUAL(0, exitCode);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_RunInteractive_NoTTY)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto session = RunWslcInteractive(std::format(L"container run -i --name {} {} cat", WslcContainerName, DebianImage.NameAndTag()));
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

    WSLC_TEST_METHOD(WSLCE2E_Container_RunAttach_TTY)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        const auto& prompt = ">";
        auto result = RunWslc(std::format(
            L"container run -itd -e PS1={} --name {} {} bash --norc", prompt, WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();

        const auto& expectedAttachPrompt = VT::BuildContainerAttachPrompt(prompt);
        const auto& expectedPrompt = VT::BuildContainerPrompt(prompt);

        auto session = RunWslcInteractive(std::format(L"container attach {}", containerId));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        // The container attach prompt appears twice.
        session.ExpectStdout(expectedAttachPrompt);
        session.ExpectStdout(expectedAttachPrompt);

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

    WSLC_TEST_METHOD(WSLCE2E_Container_RunAttach_NoTTY)
    {
        VerifyContainerIsNotListed(WslcContainerName);
        auto result = RunWslc(std::format(L"container run -id --name {} {} cat", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();

        auto session = RunWslcInteractive(std::format(L"container attach {}", containerId));
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

    WSLC_TEST_METHOD(WSLCE2E_Container_ExecInteractive_TTY)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        const auto& prompt = ">";
        auto result =
            RunWslc(std::format(L"container run -itd -e PS1={} --name {} {}", prompt, WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();

        const auto& expectedPrompt = VT::BuildContainerPrompt(prompt);

        auto session = RunWslcInteractive(std::format(L"container exec -it {} /bin/bash --norc", containerId));
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

    WSLC_TEST_METHOD(WSLCE2E_Container_ExecInteractive_NoTTY)
    {
        VerifyContainerIsNotListed(WslcContainerName);
        auto result = RunWslc(std::format(L"container run -id --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
        auto containerId = result.GetStdoutOneLine();

        auto session = RunWslcInteractive(std::format(L"container exec -i {} cat", containerId));
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

    WSLC_TEST_METHOD(WSLCE2E_Container_CreateStartAttach_TTY)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        const auto& prompt = ">";
        auto result = RunWslc(std::format(
            L"container create -it -e PS1={} --name {} {} bash --norc", prompt, WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
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

    WSLC_TEST_METHOD(WSLCE2E_Container_CreateStartAttach_NoTTY)
    {
        VerifyContainerIsNotListed(WslcContainerName);
        auto result = RunWslc(std::format(L"container create -i --name {} {} cat", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});
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

    WSLC_TEST_METHOD(WSLCE2E_Container_CreateStartAttach_ShortRunningInitProcess)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        constexpr auto ExpectedExitCode = 37;

        auto result = RunWslc(std::format(
            L"container create --name {} {} sh -c \"echo lifecycle works; exit {}\"", WslcContainerName, AlpineImage.NameAndTag(), ExpectedExitCode));

        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"lifecycle works\n", .Stderr = L"", .ExitCode = ExpectedExitCode});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Port_TCP)
    {
        // Start a container with a simple server listening on a port
        auto result = RunWslc(std::format(
            L"container run -d --name {} -p {}:{} {} {}",
            WslcContainerName,
            HostTestPort1,
            ContainerTestPort,
            PythonImage.NameAndTag(),
            GetPythonHttpServerScript(ContainerTestPort)));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify we can connect to the server from the host side
        ExpectHttpResponse(std::format(L"http://127.0.0.1:{}", HostTestPort1).c_str(), HTTP_STATUS_OK, true);

        // Verify the port mapping is correct in the container inspect data
        auto inspectContainer = InspectContainer(WslcContainerName);
        auto portKey = std::to_string(ContainerTestPort) + "/tcp";
        VERIFY_IS_TRUE(inspectContainer.Ports.contains(portKey));

        auto portBindings = inspectContainer.Ports[portKey];
        VERIFY_ARE_EQUAL(1u, portBindings.size());
        VERIFY_ARE_EQUAL(std::to_string(HostTestPort1), portBindings[0].HostPort);
        VERIFY_ARE_EQUAL("127.0.0.1", portBindings[0].HostIp);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_PortMultipleMappings)
    {
        // Start a container with a simple server listening on a port
        // Map two host ports to the same container port
        auto result = RunWslc(std::format(
            L"container run -d --name {} -p {}:{} -p {}:{} {} {}",
            WslcContainerName,
            HostTestPort1,
            ContainerTestPort,
            HostTestPort2,
            ContainerTestPort,
            PythonImage.NameAndTag(),
            GetPythonHttpServerScript(ContainerTestPort)));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // From the host side, verify we can connect to both ports
        ExpectHttpResponse(std::format(L"http://127.0.0.1:{}", HostTestPort1).c_str(), HTTP_STATUS_OK, true);
        ExpectHttpResponse(std::format(L"http://127.0.0.1:{}", HostTestPort2).c_str(), HTTP_STATUS_OK, true);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_PortAlreadyInUse)
    {
        // Bug: https://github.com/microsoft/WSL/issues/14448
        SKIP_TEST_NOT_IMPL();

        // Start a container with a simple server listening on a port
        auto result1 = RunWslc(std::format(
            L"container run -d --name {} -p {}:{} {} {}",
            WslcContainerName,
            HostTestPort1,
            ContainerTestPort,
            PythonImage.NameAndTag(),
            GetPythonHttpServerScript(ContainerTestPort)));
        result1.Verify({.Stderr = L"", .ExitCode = 0});

        // Attempt to start another container mapping the same host port
        auto result2 = RunWslc(std::format(L"container run -p {}:{} {}", HostTestPort1, ContainerTestPort, DebianImage.NameAndTag()));
        result2.Verify({.ExitCode = 1});
    }

    // https://github.com/microsoft/WSL/issues/14433
    WSLC_TEST_METHOD(WSLCE2E_Container_Run_PortEphemeral_NotSupported)
    {
        auto result = RunWslc(std::format(L"container run -p 80 {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Port mappings with ephemeral host ports, specific host IPs, or UDP protocol are not currently supported\r\nError code: ERROR_NOT_SUPPORTED\r\n", .ExitCode = 1});
    }

    // https://github.com/microsoft/WSL/issues/14433
    WSLC_TEST_METHOD(WSLCE2E_Container_Run_PortUdp_NotSupported)
    {
        auto result = RunWslc(std::format(L"container run -p 80:80/udp {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Port mappings with ephemeral host ports, specific host IPs, or UDP protocol are not currently supported\r\nError code: ERROR_NOT_SUPPORTED\r\n", .ExitCode = 1});
    }

    // https://github.com/microsoft/WSL/issues/14433
    WSLC_TEST_METHOD(WSLCE2E_Container_Run_PortHostIP_NotSupported)
    {
        auto result = RunWslc(std::format(L"container run -p 127.0.0.1:80:80 {}", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Port mappings with ephemeral host ports, specific host IPs, or UDP protocol are not currently supported\r\nError code: ERROR_NOT_SUPPORTED\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_UserOption_UidRoot)
    {
        auto result = RunWslc(
            std::format(L"container create --name {} -u 0 {} sh -c \"id -u; id -g\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"0\n0\n", .Stderr = L"", .ExitCode = S_OK});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_UserOption_NameGroupRoot)
    {
        auto result = RunWslc(std::format(
            L"container create --name {} -u root:root {} sh -c \"id -un; id -u; id -g\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"root\n0\n0\n", .Stderr = L"", .ExitCode = S_OK});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_UserOption_UnknownUser_Fails)
    {
        auto result = RunWslc(
            std::format(L"container create --name {} -u user_does_not_exist {} id -u", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify(
            {.Stderr = L"unable to find user user_does_not_exist: no matching entries in passwd file\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_UserOption_UidRoot)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec -u 0 {} sh -c \"id -u; id -g\"", WslcContainerName));
        result.Verify({.Stdout = L"0\n0\n", .Stderr = L"", .ExitCode = S_OK});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_UserOption_NameGroupRoot)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec -u root:root {} sh -c \"id -un; id -u; id -g\"", WslcContainerName));
        result.Verify({.Stdout = L"root\n0\n0\n", .Stderr = L"", .ExitCode = S_OK});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_UserOption_InvalidGroup_Fails)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec -u root:badgid {} id -u", WslcContainerName));
        result.Verify({.Stdout = L"unable to find group badgid: no matching entries in group file\r\n", .ExitCode = 126});
    }

    TEST_METHOD(WSLCE2E_Container_Create_Tmpfs)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(
            L"container create --name {} --tmpfs /wslc-tmpfs {} sh -c \"echo -n 'tmpfs_test' > /wslc-tmpfs/data && cat "
            L"/wslc-tmpfs/data\"",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"tmpfs_test", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Create_Tmpfs_With_Options)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(
            L"container create --name {} --tmpfs /wslc-tmpfs:size=64k {} sh -c \"mount | grep -q ' on /wslc-tmpfs type tmpfs ' "
            L"&& echo mounted\"",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"mounted\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Create_Tmpfs_Multiple_With_Options)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(
            L"container create --name {} --tmpfs /wslc-tmpfs1:size=64k --tmpfs /wslc-tmpfs2:size=128k {} sh -c \"mount | grep -q "
            L"' on /wslc-tmpfs1 type tmpfs ' && mount | grep -q ' on /wslc-tmpfs2 type tmpfs ' && echo mounted\"",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"mounted\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Create_Tmpfs_RelativePath_Fails)
    {
        WSL2_TEST_ONLY();

        auto result =
            RunWslc(std::format(L"container create --name {} --tmpfs wslc-tmpfs {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"invalid mount path: 'wslc-tmpfs' mount path must be absolute\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Create_Tmpfs_EmptyDestination_Fails)
    {
        WSL2_TEST_ONLY();

        auto result =
            RunWslc(std::format(L"container create --name {} --tmpfs :size=64k {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"invalid mount path: '' mount path must be absolute\r\nError code: E_FAIL\r\n", .ExitCode = 1});
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
    const TestImage& PythonImage = PythonTestImage();
    const TestImage& InvalidImage = InvalidTestImage();

    // Test ports
    const uint16_t ContainerTestPort = 8080;
    const uint16_t HostTestPort1 = 1234;
    const uint16_t HostTestPort2 = 1235;

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
                << L"  --tmpfs           Mount tmpfs to the container at the given path\r\n"
                << L"  -t,--tty          Open a TTY with the container process.\r\n"
                << L"  -u,--user         User ID for the process (name|uid|uid:gid)\r\n"
                << L"  -v,--volume       Bind mount a volume to the container\r\n"
                << L"  -h,--help         Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }

    void WriteEnvFile(const std::filesystem::path& filePath, const std::vector<std::string>& envVariableLines) const
    {
        std::ofstream envFile(filePath, std::ios::out | std::ios::trunc | std::ios::binary);
        VERIFY_IS_TRUE(envFile.is_open());
        for (const auto& line : envVariableLines)
        {
            envFile << line << "\n";
        }
        VERIFY_IS_TRUE(envFile.good());
    }

    bool ContainsOutputLine(const std::vector<std::wstring>& outputLines, const std::wstring& expectedLine) const
    {
        for (const auto& line : outputLines)
        {
            if (line == expectedLine)
            {
                return true;
            }
        }

        return false;
    }

    std::wstring GetPythonHttpServerScript(uint16_t port)
    {
        return std::format(L"python3 -m http.server {}", port);
    }
};
} // namespace WSLCE2ETests