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
        EnsureNetworkDoesNotExist(TestNetworkName);

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
        EnsureNetworkDoesNotExist(TestNetworkName);
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
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_MissingImage)
    {
        auto result = RunWslc(L"container create --name " + WslcContainerName);
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'image'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_InvalidImage)
    {
        auto session = OpenDefaultElevatedSession();

        auto [registryContainer, registryAddress] = StartLocalRegistry(*session, "", "", 5000);
        auto reference = std::format(L"{}/invalid-image:latest", registryAddress);

        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, reference));

        std::wstringstream expectedError;
        expectedError << L"Image '" << reference << L"' not found, pulling\r\n"
                      << L"manifest for " << reference << L" not found: manifest unknown: manifest unknown\r\n"
                      << L"Error code: WSLC_E_IMAGE_NOT_FOUND\r\n";
        result.Verify({.Stderr = expectedError.str(), .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Valid)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Create the container with a valid image
        auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        std::wstring containerId = result.GetStdoutOneLine();

        // Verify the container is listed with the correct status
        VerifyContainerIsListed(containerId, L"created");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_CIDFile_Valid)
    {
        // Prepare a CID file path that does not exist
        const auto cidFilePath = wsl::windows::common::filesystem::GetTempFilename();
        VERIFY_IS_TRUE(DeleteFileW(cidFilePath.c_str()));
        auto deleteCidFile = wil::scope_exit([&]() { VERIFY_IS_TRUE(DeleteFileW(cidFilePath.c_str())); });

        auto result = RunWslc(std::format(
            L"container create --cidfile \"{}\" --name {} {}", EscapePath(cidFilePath.wstring()), WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_TRUE(std::filesystem::exists(cidFilePath));
        VERIFY_ARE_EQUAL(containerId, ReadFileContent(cidFilePath.wstring()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_CIDFile_AlreadyExists)
    {
        const auto cidFilePath = wsl::windows::common::filesystem::GetTempFilename();
        auto deleteCidFile = wil::scope_exit([&]() { VERIFY_IS_TRUE(DeleteFileW(cidFilePath.c_str())); });

        auto result = RunWslc(std::format(
            L"container create --cidfile \"{}\" --name {} {}", EscapePath(cidFilePath.wstring()), WslcContainerName, DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr = std::format(L"CID file '{}' already exists\r\nError code: ERROR_FILE_EXISTS\r\n", EscapePath(cidFilePath.wstring())),
             .ExitCode = 1});

        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_DuplicateContainerName)
    {
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
        result.Verify({.Stdout = L"WSLC Volume Test", .Stderr = L"", .ExitCode = 0});
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
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

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
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

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

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Volume_RelativeHostPath)
    {
        // Create a uniquely-named subdirectory relative to the CWD and pass it as a relative path
        // to wslc. Using a GUID suffix avoids collisions on parallel runs or after a prior crash.
        // Uses "./" prefix to disambiguate from a Docker named volume name.
        GUID runId;
        THROW_IF_FAILED(CoCreateGuid(&runId));
        const auto dirName =
            L"wslc-vol-relpath-" + wsl::shared::string::GuidToString<wchar_t>(runId, wsl::shared::string::GuidToStringFlags::None);
        const auto absoluteDir = std::filesystem::current_path() / dirName;
        std::filesystem::create_directories(absoluteDir);
        auto cleanupDir = wil::scope_exit([&]() { std::filesystem::remove_all(absoluteDir); });

        const auto testFile = absoluteDir / L"reltest.txt";
        const auto relativeDir = L"./" + dirName;

        // Write a file from the host and verify the container can read it via the relative path mount.
        {
            std::ofstream out(testFile);
            VERIFY_IS_TRUE(out.is_open(), L"Failed to open test file for writing (host -> container test)");
            out << "WSLC Relative Path Test";
            VERIFY_IS_TRUE(out.good(), L"Failed to write to test file (host -> container test)");
        }

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --volume \"{}:/data:ro\" {} cat /data/reltest.txt",
            WslcContainerName,
            relativeDir,
            AlpineImage.NameAndTag()));
        result.Verify({.Stdout = L"WSLC Relative Path Test", .Stderr = L"", .ExitCode = 0});

        EnsureContainerDoesNotExist(WslcContainerName);

        // Write a file from the container and verify the host can read it back via the relative path mount.
        result = RunWslc(std::format(
            L"container run --rm --name {} --volume \"{}:/data:rw\" {} sh -c \"echo -n 'WSLC Relative Path Write Test' > "
            L"/data/reltest.txt\"",
            WslcContainerName,
            relativeDir,
            AlpineImage.NameAndTag()));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        std::ifstream in(testFile);
        VERIFY_IS_TRUE(in.is_open(), L"Failed to open test file for reading (container -> host test)");
        std::stringstream buffer;
        buffer << in.rdbuf();
        VERIFY_IS_TRUE(in.good() || in.eof(), L"Failed to read test file (container -> host test)");
        VERIFY_ARE_EQUAL("WSLC Relative Path Write Test", buffer.str());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Volume_Invalid)
    {
        {
            auto result =
                RunWslc(std::format(L"container run --name {} --volume :/containerPath {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: ':/containerPath'. Host path cannot be empty. Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(
                std::format(L"container run --name {} --volume C:\\hostPath::ro {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'C:\\hostPath::ro'. Container path cannot be empty. Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(
                std::format(L"container run --name {} --volume :/containerPath:ro {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: ':/containerPath:ro'. Host path cannot be empty. Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(L"container run --name {} --volume \"\" {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: ''. Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container run --name {} --volume C:\\hostPath: {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'C:\\hostPath:'. Container path cannot be empty. Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container run --name {} --volume C:\\hostPath:ro {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'C:\\hostPath:ro'. Container path must be an absolute path (starting with '/'). Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(L"container run --name {} --volume :ro {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: ':ro'. Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(
                std::format(L"container run --name {} --volume C:\\hostPath::rw {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'C:\\hostPath::rw'. Container path cannot be empty. Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(
                L"container run --name {} --volume C:\\hostPath:/containerPath:invalid_mode {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'C:\\hostPath:/containerPath:invalid_mode'. Container path must be an absolute path (starting with '/'). Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(
                L"container run --name {} --volume C:\\hostPath:/containerPath:ro:extra {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'C:\\hostPath:/containerPath:ro:extra'. Container path must be an absolute path (starting with '/'). Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(
                L"container run --name {} --volume C:\\hostPath:/containerPath: {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'C:\\hostPath:/containerPath:'. Container path cannot be empty. Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            // "::/container:ro" - host=":", container="/container". ":" is not a valid Windows path.
            auto result = RunWslc(
                std::format(L"container run --name {} --volume \"::/container:ro\" {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: '::/container:ro'. Host path ':' is not a valid Windows path.\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
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
            result.Verify({.Stderr = L"Invalid volume specifications: 'C:\\hostPath'. Container path must be an absolute path (starting with '/'). Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(L"container run --name {} --volume \":\" {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: ':'. Container path cannot be empty. Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            // "::" splits as host=":", container="". Container path empty check fires first.
            auto result =
                RunWslc(std::format(L"container run --name {} --volume \"::\" {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: '::'. Container path cannot be empty. Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container run --name {} --volume \"e2e_test\" {}", WslcContainerName, AlpineImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid volume specifications: 'e2e_test'. Expected format: <host path | named volume>:<container path>[:mode]\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Remove)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(L"container create --rm --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Start the container.
        result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stdout = std::format(L"{}\r\n", WslcContainerName), .Stderr = L"", .ExitCode = 0});

        // Verify with retry timeout of 1 minute.
        VerifyContainerIsNotListed(WslcContainerName, std::chrono::seconds(2), std::chrono::minutes(1));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Start_AlreadyRunning)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_FALSE(containerId.empty());

        VerifyContainerIsListed(containerId, L"running");

        // Start again - should succeed without error
        result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stdout = std::format(L"{}\r\n", WslcContainerName), .Stderr = L"", .ExitCode = 0});

        // Verify the container is still running
        VerifyContainerIsListed(containerId, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_CreateStartAttach_TTY)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        const auto& prompt = ">";
        auto result = RunWslc(std::format(
            L"container create -it -e PS1={} --name {} {} bash --norc", prompt, WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = result.GetStdoutOneLine();

        const auto& expectedPrompt = VT::BuildContainerPrompt(prompt, true);

        auto session = RunWslcInteractive(std::format(L"container start --attach {}", containerId));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        // Ignore resize-repaint messages. Those are emitted when the the tty initial size is set, which can happen before or after we start running commands.
        session.IgnoreSequence(VT::BuildContainerAttachPrompt(prompt));

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

    WSLC_TEST_METHOD(WSLCE2E_Container_CreateStartAttach_ShortRunningInitProcess)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        constexpr auto ExpectedExitCode = 37;

        auto result = RunWslc(std::format(
            L"container create --name {} {} sh -c \"echo lifecycle works; exit {}\"", WslcContainerName, AlpineImage.NameAndTag(), ExpectedExitCode));

        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"lifecycle works\n", .Stderr = L"", .ExitCode = ExpectedExitCode});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_UserOption_UidRoot)
    {
        auto result = RunWslc(
            std::format(L"container create --name {} -u 0 {} sh -c \"id -u; id -g\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_UserOption_NameGroupRoot)
    {
        auto result = RunWslc(std::format(
            L"container create --name {} -u root:root {} sh -c \"id -un; id -u; id -g\"", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"root\n0\n0\n", .Stderr = L"", .ExitCode = 0});
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Tmpfs)
    {
        auto result = RunWslc(std::format(
            L"container create --name {} --tmpfs /wslc-tmpfs {} sh -c \"echo -n 'tmpfs_test' > /wslc-tmpfs/data && cat "
            L"/wslc-tmpfs/data\"",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"tmpfs_test", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Tmpfs_With_Options)
    {
        auto result = RunWslc(std::format(
            L"container create --name {} --tmpfs /wslc-tmpfs:size=64k {} sh -c \"mount | grep -q ' on /wslc-tmpfs type tmpfs ' "
            L"&& echo mounted\"",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"mounted\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Tmpfs_Multiple_With_Options)
    {
        auto result = RunWslc(std::format(
            L"container create --name {} --tmpfs /wslc-tmpfs1:size=64k --tmpfs /wslc-tmpfs2:size=128k {} sh -c \"mount | grep -q "
            L"' on /wslc-tmpfs1 type tmpfs ' && mount | grep -q ' on /wslc-tmpfs2 type tmpfs ' && echo mounted\"",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"mounted\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Tmpfs_RelativePath_Fails)
    {
        auto result =
            RunWslc(std::format(L"container create --name {} --tmpfs wslc-tmpfs {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"invalid mount path: 'wslc-tmpfs' mount path must be absolute\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Tmpfs_EmptyDestination_Fails)
    {
        auto result =
            RunWslc(std::format(L"container create --name {} --tmpfs :size=64k {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"invalid mount path: '' mount path must be absolute\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_WorkDir)
    {
        auto result =
            RunWslc(std::format(L"container create --name {} --workdir /tmp {} pwd", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"/tmp\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_WithLabel_Success)
    {
        auto result =
            RunWslc(std::format(L"container create --name {} --label A=1 --label B=2 {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspect = InspectContainer(WslcContainerName);
        VERIFY_ARE_EQUAL("1", inspect.Labels["A"]);
        VERIFY_ARE_EQUAL("2", inspect.Labels["B"]);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Hostname)
    {
        auto result = RunWslc(std::format(
            L"container create --name {} --hostname my-test-host {} hostname", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"my-test-host\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Domainname)
    {
        auto result = RunWslc(std::format(
            L"container create --name {} --domainname my-test-domain {} dnsdomainname", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"my-test-domain\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_DNS)
    {
        auto result = RunWslc(std::format(
            L"container create --name {} --dns 1.1.1.1 --dns 8.8.8.8 {} cat /etc/resolv.conf", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout->find(L"nameserver 1.1.1.1") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(L"nameserver 8.8.8.8") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_DNSSearch)
    {
        auto result = RunWslc(std::format(
            L"container create --name {} --dns-search example.com --dns-search test.local {} cat /etc/resolv.conf",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout->find(L"search example.com test.local") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_DNSOption)
    {
        auto result = RunWslc(std::format(
            L"container create --name {} --dns-option ndots:5 --dns-option timeout:3 {} cat /etc/resolv.conf",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout->find(L"options ndots:5 timeout:3") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_StopSignal)
    {
        constexpr int ExpectedExitCode = 42;
        auto result = RunWslc(std::format(
            LR"(container create --stop-signal SIGUSR1 --name {} {} bash -c "trap 'exit {}' SIGUSR1; while true; do sleep 1; done")",
            WslcContainerName,
            DebianImage.NameAndTag(),
            ExpectedExitCode));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        const auto containerId = result.GetStdoutOneLine();
        VerifyContainerIsListed(containerId, L"created");

        result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VerifyContainerIsListed(containerId, L"running");

        result = RunWslc(std::format(L"container stop {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        VERIFY_IS_FALSE(inspect.State.Running);
        VERIFY_ARE_EQUAL(ExpectedExitCode, inspect.State.ExitCode);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_StopTimeout)
    {
        // A positive value is forwarded to the container configuration.
        {
            constexpr int ExpectedStopTimeout = 30;
            auto result = RunWslc(std::format(
                L"container create --stop-timeout {} --name {} {}", ExpectedStopTimeout, WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto inspect = InspectContainer(WslcContainerName);
            VERIFY_IS_TRUE(inspect.Config.StopTimeout.has_value());
            VERIFY_ARE_EQUAL(ExpectedStopTimeout, inspect.Config.StopTimeout.value());
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        // A value of 0 (stop the container immediately) is a valid, explicit timeout.
        {
            auto result =
                RunWslc(std::format(L"container create --stop-timeout 0 --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto inspect = InspectContainer(WslcContainerName);
            VERIFY_IS_TRUE(inspect.Config.StopTimeout.has_value());
            VERIFY_ARE_EQUAL(0, inspect.Config.StopTimeout.value());
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        // A value of -1 means "no timeout"; it is a valid, explicit value forwarded to the configuration.
        {
            auto result =
                RunWslc(std::format(L"container create --stop-timeout -1 --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto inspect = InspectContainer(WslcContainerName);
            VERIFY_IS_TRUE(inspect.Config.StopTimeout.has_value());
            VERIFY_ARE_EQUAL(-1, inspect.Config.StopTimeout.value());
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        // When --stop-timeout is not specified, no timeout is forwarded to the container configuration.
        {
            auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto inspect = InspectContainer(WslcContainerName);
            VERIFY_IS_FALSE(inspect.Config.StopTimeout.has_value());
            EnsureContainerDoesNotExist(WslcContainerName);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_StopTimeout_Invalid)
    {
        {
            auto result =
                RunWslc(std::format(L"container create --stop-timeout abc --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid stop-timeout argument value: abc\r\n", .ExitCode = 1});
            VerifyContainerIsNotListed(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container create --stop-timeout -2 --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid stop timeout value: -2\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            VerifyContainerIsNotListed(WslcContainerName);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_ShmSize)
    {
        auto result = RunWslc(
            std::format(L"container create --shm-size 128M --name {} {} df -h /dev/shm", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout->find(L"128M") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_ShmSize_Invalid)
    {
        {
            auto result =
                RunWslc(std::format(L"container create --shm-size invalid --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid shm-size argument value: 'invalid'. Expected a memory size (e.g. 256M, 1G)\r\n", .ExitCode = 1});
            VerifyContainerIsNotListed(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container create --shm-size 128X --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid shm-size argument value: '128X'. Expected a memory size (e.g. 256M, 1G)\r\n", .ExitCode = 1});
            VerifyContainerIsNotListed(WslcContainerName);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_HealthCheck)
    {
        // All health-check options are forwarded to the container configuration.
        {
            auto result = RunWslc(std::format(
                LR"(container create --health-cmd "exit 0" --health-interval 5s --health-timeout 3s --health-retries 2 --health-start-period 1s --name {} {})",
                WslcContainerName,
                DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto inspect = InspectContainer(WslcContainerName);
            VERIFY_IS_TRUE(inspect.Config.Healthcheck.has_value());

            const auto& health = inspect.Config.Healthcheck.value();
            VERIFY_IS_TRUE(health.Test.has_value());
            const std::vector<std::string> expectedTest{"CMD-SHELL", "exit 0"};
            VERIFY_ARE_EQUAL(expectedTest, health.Test.value());

            // Durations are reported in nanoseconds.
            VERIFY_IS_TRUE(health.Interval.has_value());
            VERIFY_ARE_EQUAL(5'000'000'000LL, health.Interval.value());
            VERIFY_IS_TRUE(health.Timeout.has_value());
            VERIFY_ARE_EQUAL(3'000'000'000LL, health.Timeout.value());
            VERIFY_IS_TRUE(health.StartPeriod.has_value());
            VERIFY_ARE_EQUAL(1'000'000'000LL, health.StartPeriod.value());
            VERIFY_IS_TRUE(health.Retries.has_value());
            VERIFY_ARE_EQUAL(2, health.Retries.value());

            EnsureContainerDoesNotExist(WslcContainerName);
        }

        // Only --health-cmd: the command is forwarded, other fields fall back to the default.
        {
            auto result = RunWslc(
                std::format(LR"(container create --health-cmd "exit 1" --name {} {})", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto inspect = InspectContainer(WslcContainerName);
            VERIFY_IS_TRUE(inspect.Config.Healthcheck.has_value());

            const auto& health = inspect.Config.Healthcheck.value();
            VERIFY_IS_TRUE(health.Test.has_value());
            const std::vector<std::string> expectedTest{"CMD-SHELL", "exit 1"};
            VERIFY_ARE_EQUAL(expectedTest, health.Test.value());

            EnsureContainerDoesNotExist(WslcContainerName);
        }

        // When no health option is specified, no health check is forwarded.
        {
            auto result = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto inspect = InspectContainer(WslcContainerName);
            VERIFY_IS_FALSE(inspect.Config.Healthcheck.has_value());
            EnsureContainerDoesNotExist(WslcContainerName);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_HealthCheck_Invalid)
    {
        {
            auto result = RunWslc(std::format(
                L"container create --health-interval notaduration --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify(
                {.Stderr = L"Invalid health-interval argument value: 'notaduration'. Expected a duration (e.g. 30s, 1m30s)\r\n", .ExitCode = 1});
            VerifyContainerIsNotListed(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container create --health-retries abc --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid health-retries argument value: abc\r\n", .ExitCode = 1});
            VerifyContainerIsNotListed(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(
                LR"(container create --no-healthcheck --health-cmd "exit 0" --name {} {})", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"The --no-healthcheck option cannot be combined with other health check options.\r\n", .ExitCode = 1});
            VerifyContainerIsNotListed(WslcContainerName);
        }

        {
            auto result = RunWslc(std::format(
                L"container create --no-healthcheck --health-interval 5s --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"The --no-healthcheck option cannot be combined with other health check options.\r\n", .ExitCode = 1});
            VerifyContainerIsNotListed(WslcContainerName);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_StopSignal_Invalid)
    {
        {
            auto result = RunWslc(
                std::format(L"container create --stop-signal SIGINVALID --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid stop-signal value: SIGINVALID is not a recognized signal name or number (Example: SIGKILL, kill, or 9).\r\n", .ExitCode = 1});
            VerifyContainerIsNotListed(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container create --stop-signal 0 --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid stop-signal value: 0 is out of valid range (1-31).\r\n", .ExitCode = 1});
            VerifyContainerIsNotListed(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container create --stop-signal 99 --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid stop-signal value: 99 is out of valid range (1-31).\r\n", .ExitCode = 1});
            VerifyContainerIsNotListed(WslcContainerName);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Network_DefaultIsBridge)
    {
        auto result = RunWslc(std::format(L"container create --name {} {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        VERIFY_ARE_EQUAL(std::string("bridge"), inspect.HostConfig.NetworkMode);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Network_HostMode_Rejected)
    {
        auto result =
            RunWslc(std::format(L"container create --name {} --network host {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"host mode networking is not supported\r\n", .ExitCode = 1});
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Network_HostMode_WithMultipleNetworks_Rejected)
    {
        auto result = RunWslc(std::format(
            L"container create --name {} --network bridge --network host {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"host mode networking is not supported\r\n", .ExitCode = 1});
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Network_UserDefinedNetwork)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto cleanupNetwork = wil::scope_exit([&] { EnsureNetworkDoesNotExist(TestNetworkName); });

        result = RunWslc(std::format(
            L"container create --name {} --network {} {} true", WslcContainerName, TestNetworkName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(TestNetworkName), inspect.HostConfig.NetworkMode);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Network_EmptyValue_Rejected)
    {
        auto result = RunWslc(std::format(L"container create --network \"\" --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Invalid network value: network name cannot be empty or whitespace\r\n", .ExitCode = 1});
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Network_NonexistentNetwork_Rejected)
    {
        auto result = RunWslc(
            std::format(L"container create --network does-not-exist --name {} {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Network not found: 'does-not-exist'\r\nError code: WSLC_E_NETWORK_NOT_FOUND\r\n", .ExitCode = 1});
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_NetworkAlias_Success)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto cleanupNetwork = wil::scope_exit([&] { EnsureNetworkDoesNotExist(TestNetworkName); });

        result = RunWslc(std::format(
            L"container create --name {} --network {} --network-alias db {} true", WslcContainerName, TestNetworkName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        const auto networkName = wsl::shared::string::WideToMultiByte(TestNetworkName);
        VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
        const auto& endpoint = inspect.NetworkSettings.Networks.at(networkName);
        VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, "db") != endpoint.Aliases.end());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_NetworkAlias_NoNetwork_Rejected)
    {
        auto result =
            RunWslc(std::format(L"container create --network-alias db --name {} {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr =
                 L"Network aliases require a user-defined network. Use --network to specify one.\r\nError code: E_INVALIDARG\r\n",
             .ExitCode = 1});
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_NetworkAlias_NoneMode_Rejected)
    {
        auto result = RunWslc(std::format(
            L"container create --network none --network-alias db --name {} {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr =
                 L"Network aliases require a user-defined network. Use --network to specify one.\r\nError code: E_INVALIDARG\r\n",
             .ExitCode = 1});
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_NetworkAlias_MultipleNetworks_Rejected)
    {
        auto result = RunWslc(std::format(
            L"container create --network bridge --network bridge --network-alias db --name {} {} true",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Network aliases cannot be specified when multiple networks are requested. Use a single --network argument.\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_NetworkAlias_EmptyValue_Rejected)
    {
        auto result =
            RunWslc(std::format(L"container create --network-alias \"\" --name {} {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Invalid network-alias value: network alias cannot be empty or whitespace\r\n", .ExitCode = 1});
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Cpus)
    {
        auto result = RunWslc(std::format(L"container create --name {} --cpus 0.5 {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        VERIFY_ARE_EQUAL(static_cast<int64_t>(500'000'000), inspect.HostConfig.NanoCpus);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Cpus_Invalid)
    {
        auto result = RunWslc(std::format(L"container create --cpus 0 --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Invalid cpus argument value: '0'. Expected a positive number of CPUs (e.g. 0.5, 1, 2)\r\n", .ExitCode = 1});
        EnsureContainerDoesNotExist(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Memory)
    {
        auto result =
            RunWslc(std::format(L"container create --name {} --memory 32M {} true", WslcContainerName, DebianImage.NameAndTag()));
        // stderr not asserted: some kernels emit a swap-limit warning when --memory is set.
        result.Verify({.ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        VERIFY_ARE_EQUAL(static_cast<int64_t>(32) * 1024 * 1024, inspect.HostConfig.Memory);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Memory_Invalid)
    {
        auto result =
            RunWslc(std::format(L"container create --memory invalid --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Invalid memory argument value: 'invalid'. Expected a memory size (e.g. 256M, 1G)\r\n", .ExitCode = 1});
        EnsureContainerDoesNotExist(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Ulimit)
    {
        auto result = RunWslc(std::format(
            L"container create --name {} --ulimit nofile=1024:2048 --ulimit nproc=512 {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        VERIFY_ARE_EQUAL(static_cast<size_t>(2), inspect.HostConfig.Ulimits.size());

        std::map<std::string, std::pair<int64_t, int64_t>> byName;
        for (const auto& ul : inspect.HostConfig.Ulimits)
        {
            byName[ul.Name] = {ul.Soft, ul.Hard};
        }

        VERIFY_IS_TRUE(byName.contains("nofile"));
        VERIFY_ARE_EQUAL(static_cast<int64_t>(1024), byName["nofile"].first);
        VERIFY_ARE_EQUAL(static_cast<int64_t>(2048), byName["nofile"].second);

        VERIFY_IS_TRUE(byName.contains("nproc"));
        VERIFY_ARE_EQUAL(static_cast<int64_t>(512), byName["nproc"].first);
        VERIFY_ARE_EQUAL(static_cast<int64_t>(512), byName["nproc"].second);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Ulimit_Invalid)
    {
        auto result = RunWslc(std::format(L"container create --ulimit nofile --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr = L"Invalid ulimit argument value: 'nofile'. Expected <name>=<soft>[:<hard>] (use -1 for unlimited)\r\n", .ExitCode = 1});
        EnsureContainerDoesNotExist(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Entrypoint)
    {
        auto result =
            RunWslc(std::format(L"container create --name {} --entrypoint /bin/whoami {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stdout = L"root\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_EnvFile)
    {
        WriteTestFile(
            EnvTestFile1, {"WSLC_TEST_CREATE_ENV_FILE_A=create-env-file-a", "WSLC_TEST_CREATE_ENV_FILE_B=create-env-file-b"});

        auto result = RunWslc(std::format(
            L"container create --name {} --env-file {} {} env", WslcContainerName, EscapePath(EnvTestFile1.wstring()), DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start -a {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_CREATE_ENV_FILE_A=create-env-file-a"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_CREATE_ENV_FILE_B=create-env-file-b"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_EnvFile_MissingFile)
    {
        auto result = RunWslc(std::format(
            L"container create --name {} --env-file ENV_FILE_NOT_FOUND {} env", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr = L"Environment file 'ENV_FILE_NOT_FOUND' cannot be opened for reading\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
        EnsureContainerDoesNotExist(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_EnvFile_InvalidContent)
    {
        WriteTestFile(EnvTestFile1, {"WSLC_TEST_ENV_VALID=ok", "BAD KEY=value"});

        auto result = RunWslc(std::format(
            L"container create --name {} --env-file {} {} env", WslcContainerName, EscapePath(EnvTestFile1.wstring()), DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Environment variable key 'BAD KEY' cannot contain whitespace\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
        EnsureContainerDoesNotExist(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Publish_TCP)
    {
        // Port bindings only show up in inspect after start, so create then start before inspecting.
        auto result = RunWslc(std::format(
            L"container create --name {} -p {}:{} {} sleep 5", WslcContainerName, HostTestPort1, ContainerTestPort, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the port mapping is correct in the container inspect data
        const auto inspect = InspectContainer(WslcContainerName);
        const auto portKey = std::to_string(ContainerTestPort) + "/tcp";
        VERIFY_IS_TRUE(inspect.Ports.contains(portKey));

        const auto& bindings = inspect.Ports.at(portKey);
        VERIFY_ARE_EQUAL(1u, bindings.size());
        VERIFY_ARE_EQUAL(std::to_string(HostTestPort1), bindings[0].HostPort);
        VERIFY_ARE_EQUAL("127.0.0.1", bindings[0].HostIp);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Publish_MultipleMappings)
    {
        // Map two host ports to the same container port.
        auto result = RunWslc(std::format(
            L"container create --name {} -p {}:{} -p {}:{} {} sleep 5",
            WslcContainerName,
            HostTestPort1,
            ContainerTestPort,
            HostTestPort2,
            ContainerTestPort,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Both host ports should be bound to the same container port.
        const auto inspect = InspectContainer(WslcContainerName);
        const auto portKey = std::to_string(ContainerTestPort) + "/tcp";
        VERIFY_IS_TRUE(inspect.Ports.contains(portKey));

        const auto& bindings = inspect.Ports.at(portKey);
        VERIFY_ARE_EQUAL(2u, bindings.size());

        bool foundHostPort1 = false;
        bool foundHostPort2 = false;
        for (const auto& binding : bindings)
        {
            if (binding.HostPort == std::to_string(HostTestPort1))
            {
                foundHostPort1 = true;
            }
            else if (binding.HostPort == std::to_string(HostTestPort2))
            {
                foundHostPort2 = true;
            }
        }
        VERIFY_IS_TRUE(foundHostPort1);
        VERIFY_IS_TRUE(foundHostPort2);
    }

    // https://github.com/microsoft/WSL/issues/14433
    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Publish_Ephemeral)
    {
        // -p <containerPort> (no host port) means the host picks a random port.
        auto result = RunWslc(std::format(
            L"container create --name {} -p {} {} sleep 5", WslcContainerName, ContainerTestPort, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Inspect the container to verify a host port was allocated.
        const auto inspect = InspectContainer(WslcContainerName);
        const auto portKey = std::to_string(ContainerTestPort) + "/tcp";
        VERIFY_IS_TRUE(inspect.Ports.contains(portKey));

        const auto& bindings = inspect.Ports.at(portKey);
        VERIFY_ARE_EQUAL(1u, bindings.size());
        VERIFY_IS_TRUE(std::stoi(bindings[0].HostPort) > 0);
    }

    // https://github.com/microsoft/WSL/issues/14433
    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Publish_UDP)
    {
        // Port bindings only show up in inspect after start, so create then start before inspecting.
        auto result = RunWslc(std::format(
            L"container create --name {} -p {}:{}/udp {} sleep 5",
            WslcContainerName,
            HostTestPort1,
            ContainerTestPort,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the UDP port mapping is correct in the container inspect data.
        const auto inspect = InspectContainer(WslcContainerName);
        const auto portKey = std::to_string(ContainerTestPort) + "/udp";
        VERIFY_IS_TRUE(inspect.Ports.contains(portKey));

        const auto& bindings = inspect.Ports.at(portKey);
        VERIFY_ARE_EQUAL(1u, bindings.size());
        VERIFY_ARE_EQUAL(std::to_string(HostTestPort1), bindings[0].HostPort);
        VERIFY_ARE_EQUAL("127.0.0.1", bindings[0].HostIp);
    }

    // https://github.com/microsoft/WSL/issues/14433
    WSLC_TEST_METHOD(WSLCE2E_Container_Create_Publish_HostIP)
    {
        // Port bindings only show up in inspect after start, so create then start before inspecting.
        auto result = RunWslc(std::format(
            L"container create --name {} -p 127.0.0.1:{}:{} {} sleep 5",
            WslcContainerName,
            HostTestPort1,
            ContainerTestPort,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container start {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the port mapping is bound to the requested host IP in the container inspect data.
        const auto inspect = InspectContainer(WslcContainerName);
        const auto portKey = std::to_string(ContainerTestPort) + "/tcp";
        VERIFY_IS_TRUE(inspect.Ports.contains(portKey));

        const auto& bindings = inspect.Ports.at(portKey);
        VERIFY_ARE_EQUAL(1u, bindings.size());
        VERIFY_ARE_EQUAL(std::to_string(HostTestPort1), bindings[0].HostPort);
        VERIFY_ARE_EQUAL("127.0.0.1", bindings[0].HostIp);
    }

private:
    // Test container name
    const std::wstring WslcContainerName = L"wslc-test-container";

    // Test network name
    const std::wstring TestNetworkName = L"wslc-test-network";

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
                 << L"  image                  Image name\r\n"
                 << L"  command                The command to run\r\n"
                 << L"  arguments              Arguments to pass to container's init process\r\n\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options
            << L"The following options are available:\r\n" //
            << L"  --cidfile              Write the container ID to the provided path\r\n"
            << L"  --cpus                 Number of CPUs (e.g. 0.5, 1, 2.5)\r\n"
            << L"  --dns                  IP address of the DNS nameserver in resolv.conf\r\n"
            << L"  --dns-option           Set DNS options\r\n"
            << L"  --dns-search           Set DNS search domains\r\n"
            << L"  --domainname           Container domain name\r\n"
            << L"  --entrypoint           Specifies the container init process executable\r\n"
            << L"  -e,--env               Key=Value pairs for environment variables\r\n"
            << L"  --env-file             File containing key=value pairs of env variables\r\n"
            << L"  --gpus                 Add GPU devices to the container ('all' to pass all GPUs)\r\n"
            << L"  --health-cmd           Command to run to check container health\r\n"
            << L"  --health-interval      Time between running the health check (e.g. 30s, 1m30s)\r\n"
            << L"  --health-retries       Consecutive failures needed to report the container as unhealthy\r\n"
            << L"  --health-start-period  Start period for the container to initialize before health-check countdown (e.g. 30s, "
               L"1m30s)\r\n"
            << L"  --health-timeout       Maximum time to allow one health check to run (e.g. 30s, 1m30s)\r\n"
            << L"  -h,--hostname          Container host name\r\n"
            << L"  -i,--interactive       Attach to stdin and keep it open\r\n"
            << L"  -l,--label             Set metadata on an object\r\n"
            << L"  -m,--memory            Memory limit (e.g. 512M, 1G)\r\n"
            << L"  --name                 Name of the container\r\n"
            << L"  --network              Connect a container to a network\r\n"
            << L"  --network-alias        Add a network-scoped alias for the container\r\n"
            << L"  --no-healthcheck       Disable any container-specified health check\r\n"
            << L"  -p,--publish           Publish a port from a container to host\r\n"
            << L"  -P,--publish-all       Publish all exposed ports to random host ports\r\n"
            << L"  --rm                   Remove the container after it stops\r\n"
            << L"  --shm-size             Size of /dev/shm (e.g. 64M, 1G)\r\n"
            << L"  --stop-signal          Signal to stop the container\r\n"
            << L"  --stop-timeout         Timeout (in seconds) to stop the container before killing it (-1 for no timeout)\r\n"
            << L"  --tmpfs                Mount tmpfs to the container at the given path\r\n"
            << L"  -t,--tty               Open a TTY with the container process.\r\n"
            << L"  --ulimit               Ulimit options (format: <name>=<soft>[:<hard>], use -1 for unlimited)\r\n"
            << L"  -u,--user              User ID for the process (name|uid|uid:gid)\r\n"
            << L"  -v,--volume            Bind mount a volume to the container\r\n"
            << L"  -w,--workdir           Working directory inside the container\r\n"
            << L"  -?,--help              Shows help about the selected command\r\n"
            << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
