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
    WSLC_TEST_CLASS(WSLCE2EContainerRunTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        EnsureImageIsLoaded(PythonImage);

        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName.c_str(), HostEnvVariableValue.c_str()));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName2.c_str(), HostEnvVariableValue2.c_str()));

        // Initialize Winsock for loopback connectivity tests
        WSADATA wsaData{};
        const int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        THROW_HR_IF(HRESULT_FROM_WIN32(result), result != 0);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(WslcContainerName2);
        EnsureImageIsDeleted(DebianImage);
        EnsureImageIsDeleted(PythonImage);
        EnsureVolumeDoesNotExist(WslcVolumeName);
        EnsureNetworkDoesNotExist(TestNetworkName);

        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName.c_str(), nullptr));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName2.c_str(), nullptr));

        // Cleanup Winsock
        WSACleanup();
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureContainerDoesNotExist(WslcContainerName2);
        EnsureVolumeDoesNotExist(WslcVolumeName);
        EnsureNetworkDoesNotExist(TestNetworkName);

        EnvTestFile1 = wsl::windows::common::filesystem::GetTempFilename();
        EnvTestFile2 = wsl::windows::common::filesystem::GetTempFilename();
        return true;
    }

    TEST_METHOD_CLEANUP(TestMethodCleanup)
    {
        DeleteFileW(EnvTestFile1.c_str());
        DeleteFileW(EnvTestFile2.c_str());
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_HelpCommand)
    {
        auto result = RunWslc(L"container run --help");
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_FALSE(result.Stdout.value().empty());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Container_With_Command)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto command = L"echo echo_from_container";
        auto result = RunWslc(std::format(L"container run --name {} {} {}", WslcContainerName, DebianImage.NameAndTag(), command));
        result.Verify({.Stdout = L"echo_from_container\n", .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(WslcContainerName, L"exited");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_CIDFile_Valid)
    {
        // Prepare a CID file path that does not exist
        const auto cidFilePath = wsl::windows::common::filesystem::GetTempFilename();
        VERIFY_IS_TRUE(DeleteFileW(cidFilePath.c_str()));
        auto deleteCidFile = wil::scope_exit([&]() { VERIFY_IS_TRUE(DeleteFileW(cidFilePath.c_str())); });

        auto result = RunWslc(std::format(
            L"container run -d --cidfile \"{}\" --name {} {} sleep infinity",
            EscapePath(cidFilePath.wstring()),
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto containerId = result.GetStdoutOneLine();
        VERIFY_IS_TRUE(std::filesystem::exists(cidFilePath));
        VERIFY_ARE_EQUAL(containerId, ReadFileContent(cidFilePath.wstring()));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_CIDFile_AlreadyExists)
    {
        const auto cidFilePath = wsl::windows::common::filesystem::GetTempFilename();
        auto deleteCidFile = wil::scope_exit([&]() { VERIFY_IS_TRUE(DeleteFileW(cidFilePath.c_str())); });

        auto result = RunWslc(std::format(
            L"container run --cidfile \"{}\" --name {} {}", EscapePath(cidFilePath.wstring()), WslcContainerName, DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr = std::format(L"CID file '{}' already exists\r\nError code: ERROR_FILE_EXISTS\r\n", EscapePath(cidFilePath.wstring())),
             .ExitCode = 1});

        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Entrypoint)
    {
        auto result = RunWslc(std::format(L"container run --rm --entrypoint /bin/whoami {}", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"root\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Entrypoint_And_Arguments)
    {
        auto result = RunWslc(
            std::format(L"container run --rm --entrypoint /bin/echo {} hello from entrypoint with args", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"hello from entrypoint with args\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Entrypoint_Invalid_Path)
    {
        auto result = RunWslc(std::format(L"container run --rm --entrypoint /bin/does-not-exist {}", DebianImage.NameAndTag()));
        result.Verify(
            {.Stdout = L"", .Stderr = L"failed to create task for container: failed to create shim task: OCI runtime create failed: runc create failed: unable to start container process: error during container init: exec: \"/bin/does-not-exist\": stat /bin/does-not-exist: no such file or directory: unknown\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Entrypoint_Detach_Lifecycle)
    {
        auto result = RunWslc(std::format(
            L"container run --name {} -d --entrypoint /bin/sleep {} infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(WslcContainerName, L"running");
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Remove)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        // Run the container with a valid image
        auto result = RunWslc(std::format(L"container run --rm --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Run should be deleted on return so no retry.
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvOption)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {}=A {} env", WslcContainerName, HostEnvVariableName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}=A", HostEnvVariableName)));
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
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}=A", HostEnvVariableName)));
        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}=B", HostEnvVariableName2)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvOption_KeyOnly_UsesHostValue)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {} {} env", WslcContainerName, HostEnvVariableName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}={}", HostEnvVariableName, HostEnvVariableValue)));
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
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}={}", HostEnvVariableName, HostEnvVariableValue)));
        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}={}", HostEnvVariableName2, HostEnvVariableValue2)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvOption_EmptyValue)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {}= {} env", WslcContainerName, HostEnvVariableName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}=", HostEnvVariableName)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvFile)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        WriteTestFile(EnvTestFile1, {"WSLC_TEST_ENV_FILE_A=env-file-a", "WSLC_TEST_ENV_FILE_B=env-file-b"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_FILE_A=env-file-a"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_FILE_B=env-file-b"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvOption_MixedWithEnvFile)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        WriteTestFile(EnvTestFile1, {"WSLC_TEST_ENV_MIX_FILE_A=from-file-a", "WSLC_TEST_ENV_MIX_FILE_B=from-file-b"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e WSLC_TEST_ENV_MIX_CLI=from-cli --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_MIX_FILE_A=from-file-a"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_MIX_FILE_B=from-file-b"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_MIX_CLI=from-cli"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvFile_MultipleFiles)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        WriteTestFile(EnvTestFile1, {"WSLC_TEST_ENV_FILE_MULTI_A=file1-a", "WSLC_TEST_ENV_FILE_MULTI_B=file1-b"});

        WriteTestFile(EnvTestFile2, {"WSLC_TEST_ENV_FILE_MULTI_C=file2-c", "WSLC_TEST_ENV_FILE_MULTI_D=file2-d"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            EscapePath(EnvTestFile2.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_FILE_MULTI_A=file1-a"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_FILE_MULTI_B=file1-b"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_FILE_MULTI_C=file2-c"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_FILE_MULTI_D=file2-d"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvFile_MissingFile)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file ENV_FILE_NOT_FOUND {} env", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            L"Environment file 'ENV_FILE_NOT_FOUND' cannot be opened for reading\r\nError code: E_INVALIDARG"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvFile_InvalidContent)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        WriteTestFile(EnvTestFile1, {"WSLC_TEST_ENV_VALID=ok", "BAD KEY=value"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            L"Environment variable key 'BAD KEY' cannot contain whitespace\r\nError code: E_INVALIDARG"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvFile_DuplicateKeys_Precedence)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        WriteTestFile(EnvTestFile1, {"WSLC_TEST_ENV_DUP=from-file-1"});

        WriteTestFile(EnvTestFile2, {"WSLC_TEST_ENV_DUP=from-file-2"});

        // Later --env-file should win over earlier --env-file for duplicate keys
        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            EscapePath(EnvTestFile2.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_DUP=from-file-2"));

        // Explicit -e should win over env-file value for duplicate keys
        result = RunWslc(std::format(
            L"container run --rm --name {} -e WSLC_TEST_ENV_DUP=from-cli --env-file {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            EscapePath(EnvTestFile2.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_DUP=from-cli"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_EnvFile_ValueContainsEquals)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        WriteTestFile(EnvTestFile1, {"WSLC_TEST_ENV_EQUALS=value=with=equals"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_EQUALS=value=with=equals"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_NameRoot)
    {
        auto result = RunWslc(std::format(L"container run --rm -u root {} sh -c \"id -un; id -u; id -g\"", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"root\n0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_UidRoot)
    {
        auto result = RunWslc(std::format(L"container run --rm -u 0 {} id -u", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"0\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_UidGidRoot)
    {
        auto result = RunWslc(std::format(L"container run --rm -u 0:0 {} sh -c \"id -u; id -g\"", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_UnknownUser_Fails)
    {
        auto result = RunWslc(std::format(L"container run --rm -u user_does_not_exist {} id -u", DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr = L"unable to find user user_does_not_exist: no matching entries in passwd file\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_UnknownGroup_Fails)
    {
        auto result = RunWslc(std::format(L"container run --rm -u root:badgid {} id -u", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"unable to find group badgid: no matching entries in group file\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_NameGroupRoot)
    {
        auto result =
            RunWslc(std::format(L"container run --rm -u root:root {} sh -c \"id -un; id -u; id -g\"", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"root\n0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_UserOption_NonRootUser_Succeeds)
    {
        auto result = RunWslc(std::format(L"container run --rm -u nobody {} sh -c \"id -un; id -u; id -g\"", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"nobody\n65534\n65534\n", .Stderr = L"", .ExitCode = 0});
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

        WaitForContainerOutput(WslcContainerName, "Serving HTTP on");

        // From the host side, verify we can connect to both ports
        ExpectHttpResponse(std::format(L"http://127.0.0.1:{}", HostTestPort1).c_str(), HTTP_STATUS_OK, true);
        ExpectHttpResponse(std::format(L"http://127.0.0.1:{}", HostTestPort2).c_str(), HTTP_STATUS_OK, true);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_PortAlreadyInUse)
    {
        // Start a container with a simple server listening on a port
        auto result1 = RunWslc(std::format(
            L"container run -d --name {} -p {}:{} {} {}",
            WslcContainerName,
            HostTestPort1,
            ContainerTestPort,
            PythonImage.NameAndTag(),
            GetPythonHttpServerScript(ContainerTestPort)));
        result1.Verify({.Stderr = L"", .ExitCode = 0});

        // Create a second container mapping the same host port to validate the full error message
        auto createResult =
            RunWslc(std::format(L"container create -p {}:{} {}", HostTestPort1, ContainerTestPort, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});
        auto containerId = createResult.GetStdoutOneLine();

        // Attempt to start — should fail with port conflict
        auto startResult = RunWslc(std::format(L"container start {}", containerId));
        startResult.Verify(
            {.Stderr = std::format(
                 L"Failed to map port '127.0.0.1:{}/tcp', Only one usage of each socket address (protocol/network "
                 L"address/port) is normally permitted. \r\nError code: WSAEADDRINUSE\r\n",
                 HostTestPort1),
             .ExitCode = 1});

        // Clean up the created container
        RunWslc(std::format(L"container rm {}", containerId)).Verify({.Stderr = L"", .ExitCode = 0});

        // Verify 'container run' auto-cleans up on port conflict (no ghost container)
        auto runResult = RunWslc(std::format(
            L"container run --name {} -p {}:{} {}", WslcContainerName2, HostTestPort1, ContainerTestPort, DebianImage.NameAndTag()));
        runResult.Verify({.ExitCode = 1});

        VerifyContainerIsNotListed(WslcContainerName2);

        // Repeat the conflict scenario for an IPv6 loopback ([::1]) binding to validate the IPv6 error message.
        auto ipv6Server = RunWslc(std::format(
            L"container run -d --name {} -p [::1]:{}:{} {} {}",
            WslcContainerName2,
            HostTestPort2,
            ContainerTestPort,
            PythonImage.NameAndTag(),
            GetPythonHttpServerScript(ContainerTestPort)));
        ipv6Server.Verify({.Stderr = L"", .ExitCode = 0});

        // Create a second container mapping the same IPv6 address/port to validate the full error message.
        auto ipv6CreateResult =
            RunWslc(std::format(L"container create -p [::1]:{}:{} {}", HostTestPort2, ContainerTestPort, DebianImage.NameAndTag()));
        ipv6CreateResult.Verify({.Stderr = L"", .ExitCode = 0});
        auto ipv6ContainerId = ipv6CreateResult.GetStdoutOneLine();

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            RunWslc(std::format(L"container rm {}", ipv6ContainerId)).Verify({.Stderr = L"", .ExitCode = 0});
        });

        // Attempt to start — should fail with a port conflict, with the IPv6 address bracketed in the message.
        auto ipv6StartResult = RunWslc(std::format(L"container start {}", ipv6ContainerId));
        ipv6StartResult.Verify(
            {.Stderr = std::format(
                 L"Failed to map port '[::1]:{}/tcp', Only one usage of each socket address (protocol/network "
                 L"address/port) is normally permitted. \r\nError code: WSAEADDRINUSE\r\n",
                 HostTestPort2),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_PortEphemeral)
    {
        // Start a container with an ephemeral host port mapping (-p 8080 means host picks a random port)
        auto result = RunWslc(std::format(
            L"container run -d --name {} -p {} {} {}", WslcContainerName, ContainerTestPort, PythonImage.NameAndTag(), GetPythonHttpServerScript(ContainerTestPort)));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Wait for the in-container HTTP server to start listening before connecting.
        WaitForContainerOutput(WslcContainerName, "Serving HTTP on");

        // Inspect the container to find the allocated host port
        auto inspectContainer = InspectContainer(WslcContainerName);
        auto portKey = std::to_string(ContainerTestPort) + "/tcp";
        VERIFY_IS_TRUE(inspectContainer.Ports.contains(portKey));

        auto portBindings = inspectContainer.Ports[portKey];
        VERIFY_ARE_EQUAL(1u, portBindings.size());

        auto hostPort = std::stoi(portBindings[0].HostPort);
        VERIFY_IS_TRUE(hostPort > 0);

        // Verify we can connect to the server on the ephemeral port
        ExpectHttpResponse(std::format(L"http://127.0.0.1:{}", hostPort).c_str(), HTTP_STATUS_OK, true);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Port_UDP)
    {
        // Start a container with a UDP echo server listening on a port.
        auto result = RunWslc(std::format(
            L"container run -d --name {} -p {}:{}/udp {} {}",
            WslcContainerName,
            HostTestPort1,
            ContainerTestPort,
            PythonImage.NameAndTag(),
            GetPythonUdpEchoServerScript(ContainerTestPort)));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Send a datagram from the host and verify the container echoes it back uppercased.
        SendUdpAndReceive(HostTestPort1, "hello", "HELLO");

        // Verify the UDP port mapping is reflected in the container inspect data.
        auto inspectContainer = InspectContainer(WslcContainerName);
        auto portKey = std::to_string(ContainerTestPort) + "/udp";
        VERIFY_IS_TRUE(inspectContainer.Ports.contains(portKey));

        auto portBindings = inspectContainer.Ports[portKey];
        VERIFY_ARE_EQUAL(1u, portBindings.size());
        VERIFY_ARE_EQUAL(std::to_string(HostTestPort1), portBindings[0].HostPort);
        VERIFY_ARE_EQUAL("127.0.0.1", portBindings[0].HostIp);
    }

    // https://github.com/microsoft/WSL/issues/14433
    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Port_HostIP)
    {
        // Start a container with a server listening on a port, bound to a specific host IP (127.0.0.1).
        auto result = RunWslc(std::format(
            L"container run -d --name {} -p 127.0.0.1:{}:{} {} {}",
            WslcContainerName,
            HostTestPort1,
            ContainerTestPort,
            PythonImage.NameAndTag(),
            GetPythonHttpServerScript(ContainerTestPort)));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        WaitForContainerOutput(WslcContainerName, "Serving HTTP on");

        ExpectHttpResponse(std::format(L"http://127.0.0.1:{}", HostTestPort1).c_str(), HTTP_STATUS_OK, true);

        auto inspectContainer = InspectContainer(WslcContainerName);
        auto portKey = std::to_string(ContainerTestPort) + "/tcp";
        VERIFY_IS_TRUE(inspectContainer.Ports.contains(portKey));

        auto portBindings = inspectContainer.Ports[portKey];
        VERIFY_ARE_EQUAL(1u, portBindings.size());
        VERIFY_ARE_EQUAL(std::to_string(HostTestPort1), portBindings[0].HostPort);
        VERIFY_ARE_EQUAL("127.0.0.1", portBindings[0].HostIp);
    }

    // Verifies that 'session.defaultBindingAddress: default' resolves to the built-in
    // loopback default (127.0.0.1) for a published port specified without an explicit host IP.
    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Port_DefaultBindingAddress_Default)
    {
        const auto settingsPath = wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wslc" / L"settings.yaml";
        HostFileChange settings(settingsPath, "session:\n  defaultBindingAddress: default\n");

        auto result = RunWslc(std::format(
            L"container run -d --name {} -p {}:{} {} {}",
            WslcContainerName,
            HostTestPort1,
            ContainerTestPort,
            PythonImage.NameAndTag(),
            GetPythonHttpServerScript(ContainerTestPort)));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        WaitForContainerOutput(WslcContainerName, "Serving HTTP on");

        ExpectHttpResponse(std::format(L"http://127.0.0.1:{}", HostTestPort1).c_str(), HTTP_STATUS_OK, true);

        auto inspectContainer = InspectContainer(WslcContainerName);
        auto portKey = std::to_string(ContainerTestPort) + "/tcp";
        VERIFY_IS_TRUE(inspectContainer.Ports.contains(portKey));

        auto portBindings = inspectContainer.Ports[portKey];
        VERIFY_ARE_EQUAL(1u, portBindings.size());
        VERIFY_ARE_EQUAL(std::to_string(HostTestPort1), portBindings[0].HostPort);
        VERIFY_ARE_EQUAL("127.0.0.1", portBindings[0].HostIp);
    }

    // Verifies that a configured 'session.defaultBindingAddress' overrides the loopback
    // default for a published port specified without an explicit host IP.
    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Port_DefaultBindingAddress_Override)
    {
        const auto hostIp = GetHostAdapterIpv4();
        if (!hostIp.has_value())
        {
            WEX::Logging::Log::Comment(L"Skipping: no suitable non-loopback host IPv4 address was found.");
            return;
        }

        const auto settingsPath = wsl::windows::common::filesystem::GetLocalAppDataPath(nullptr) / L"wslc" / L"settings.yaml";
        HostFileChange settings(settingsPath, std::format("session:\n  defaultBindingAddress: {}\n", *hostIp));

        auto result = RunWslc(std::format(
            L"container run -d --name {} -p {}:{} {} {}",
            WslcContainerName,
            HostTestPort1,
            ContainerTestPort,
            PythonImage.NameAndTag(),
            GetPythonHttpServerScript(ContainerTestPort)));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        WaitForContainerOutput(WslcContainerName, "Serving HTTP on");

        const auto hostIpWide = std::wstring(hostIp->begin(), hostIp->end());
        ExpectHttpResponse(std::format(L"http://{}:{}", hostIpWide, HostTestPort1).c_str(), HTTP_STATUS_OK, true);

        auto inspectContainer = InspectContainer(WslcContainerName);
        auto portKey = std::to_string(ContainerTestPort) + "/tcp";
        VERIFY_IS_TRUE(inspectContainer.Ports.contains(portKey));

        auto portBindings = inspectContainer.Ports[portKey];
        VERIFY_ARE_EQUAL(1u, portBindings.size());
        VERIFY_ARE_EQUAL(std::to_string(HostTestPort1), portBindings[0].HostPort);
        VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(*hostIp), portBindings[0].HostIp);
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

        // Wait for the in-container HTTP server to start listening before connecting.
        WaitForContainerOutput(WslcContainerName, "Serving HTTP on");

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

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Interactive_TTY)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        const auto& prompt = ">";
        auto session = RunWslcInteractive(
            std::format(L"container run -it -e PS1={} --name {} {} bash --norc", prompt, WslcContainerName, DebianImage.NameAndTag()));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        // Ignore resize-repaint messages. Those are emitted when the the tty initial size is set, which can happen before or after we start running commands.
        session.IgnoreSequence(VT::BuildContainerAttachPrompt(prompt));

        const auto& expectedPrompt = VT::BuildContainerPrompt(prompt, true);
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Interactive_NoTTY)
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_InteractiveNoTTY_SelfExitingCommand)
    {
        // Same stdin-relay teardown deadlock as WSLCE2E_Container_Exec_InteractiveNoTTY_SelfExitingCommand (see it
        // for the root cause), but via `container run -i`. run and exec share the client relay (both route through
        // AttachToCurrentConsole), so the hang is not exec-specific. The test requires run to exit with the client
        // still holding stdin open; RunWslcInteractive supplies stdin as a synchronous (non-overlapped) pipe, the
        // case that triggers the bug.
        VerifyContainerIsNotListed(WslcContainerName);

        auto session =
            RunWslcInteractive(std::format(L"container run -i --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));

        session.ExpectStdout("hello\n");

        // Generous timeout: it only bounds the failure (hang) path.
        auto exitCode = session.Wait(120000);
        VERIFY_ARE_EQUAL(0, exitCode, L"echo should exit with code 0 without the client closing stdin");

        // Closing stdin after exit must stay a clean no-op.
        session.CloseStdin();
        session.VerifyNoErrors();
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_InteractiveTTY_SelfExitingCommand)
    {
        // TTY counterpart of the above: `-t` routes through ConsoleService::RelayInteractiveTty, a separate
        // stdin-worker teardown from the non-TTY path, so it could regress independently. Guards the TTY run path.
        VerifyContainerIsNotListed(WslcContainerName);

        auto session =
            RunWslcInteractive(std::format(L"container run -it --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));

        // The TTY translates the trailing LF to CRLF.
        session.ExpectStdout("hello\r\n");

        auto exitCode = session.Wait(120000);
        VERIFY_ARE_EQUAL(0, exitCode, L"echo should exit with code 0 without the client closing stdin");

        session.CloseStdin();
        session.VerifyNoErrors();
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_PseudoConsole_TerminalSize)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        constexpr SHORT columns = 42;
        constexpr SHORT rows = 43;
        const auto commandLine = std::format(
            L"container run --rm -it --name {} {} /bin/sh -c \"while true; do stty size; sleep 1; done\"",
            WslcContainerName,
            DebianImage.NameAndTag());

        auto session = RunWslcInteractive(commandLine, ElevationType::Elevated, PseudoConsole{columns, rows});
        VerifyPseudoConsoleTtySize(session, columns, rows);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Tmpfs)
    {
        auto result = RunWslc(std::format(
            L"container run --rm --tmpfs /wslc-tmpfs {} sh -c \"echo -n 'tmpfs_test' > /wslc-tmpfs/data && cat "
            L"/wslc-tmpfs/data\"",
            DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"tmpfs_test", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Tmpfs_With_Options)
    {
        auto result = RunWslc(std::format(
            L"container run --rm --tmpfs /wslc-tmpfs:size=64k {} sh -c \"mount | grep -q ' on /wslc-tmpfs type tmpfs ' && echo "
            L"mounted\"",
            DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"mounted\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Tmpfs_Multiple_With_Options)
    {
        auto result = RunWslc(std::format(
            L"container run --rm --tmpfs /wslc-tmpfs1:size=64k --tmpfs /wslc-tmpfs2:size=128k {} sh -c \"mount | grep -q ' on "
            L"/wslc-tmpfs1 type tmpfs ' && mount | grep -q ' on /wslc-tmpfs2 type tmpfs ' && echo mounted\"",
            DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"mounted\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Tmpfs_RelativePath_Fails)
    {
        auto result = RunWslc(std::format(L"container run --rm --tmpfs wslc-tmpfs {}", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            L"invalid mount path: 'wslc-tmpfs' mount path must be absolute\r\nError code: E_FAIL"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Tmpfs_EmptyDestination_Fails)
    {
        auto result = RunWslc(std::format(L"container run --rm --tmpfs :size=64k {}", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(
            result.StderrContainsSubstring(L"invalid mount path: '' mount path must be absolute\r\nError code: E_FAIL"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_WorkDir)
    {
        auto result = RunWslc(std::format(L"container run --rm --workdir /tmp {} pwd", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"/tmp\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Hostname)
    {
        auto result = RunWslc(std::format(L"container run --rm --hostname my-test-host {} hostname", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"my-test-host\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Domainname)
    {
        auto result = RunWslc(std::format(L"container run --rm --domainname my-test-domain {} dnsdomainname", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"my-test-domain\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_DNS)
    {
        auto result =
            RunWslc(std::format(L"container run --rm --dns 1.1.1.1 --dns 8.8.8.8 {} cat /etc/resolv.conf", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout->find(L"nameserver 1.1.1.1") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(L"nameserver 8.8.8.8") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_DNSSearch)
    {
        auto result = RunWslc(std::format(
            L"container run --rm --dns-search example.com --dns-search test.local {} cat /etc/resolv.conf", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout->find(L"search example.com test.local") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_DNSOption)
    {
        auto result = RunWslc(std::format(
            L"container run --rm --dns-option ndots:5 --dns-option timeout:3 {} cat /etc/resolv.conf", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout->find(L"options ndots:5 timeout:3") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Network_DefaultIsBridge)
    {
        auto result = RunWslc(std::format(L"container run --name {} {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        VERIFY_ARE_EQUAL(std::string("bridge"), inspect.HostConfig.NetworkMode);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Network_HostMode_Rejected)
    {
        auto result =
            RunWslc(std::format(L"container run --name {} --network host {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"host mode networking is not supported"));
        VerifyContainerIsNotListed(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Network_UserDefinedNetwork)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto cleanupNetwork = wil::scope_exit([&] { EnsureNetworkDoesNotExist(TestNetworkName); });

        result = RunWslc(std::format(
            L"container run --name {} --network {} {} true", WslcContainerName, TestNetworkName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        VERIFY_ARE_EQUAL(wsl::shared::string::WideToMultiByte(TestNetworkName), inspect.HostConfig.NetworkMode);
        VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(wsl::shared::string::WideToMultiByte(TestNetworkName)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Network_EmptyValue_Rejected)
    {
        auto result =
            RunWslc(std::format(L"container run --rm --network \"\" --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Invalid network value: network name cannot be empty or whitespace"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Network_NonexistentNetwork_Rejected)
    {
        auto result = RunWslc(std::format(
            L"container run --rm --network does-not-exist --name {} {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Network not found: 'does-not-exist'\r\nError code: WSLC_E_NETWORK_NOT_FOUND\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_NetworkAlias_Success)
    {
        auto result = RunWslc(std::format(L"network create --driver bridge {}", TestNetworkName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        auto cleanupNetwork = wil::scope_exit([&] { EnsureNetworkDoesNotExist(TestNetworkName); });

        result = RunWslc(std::format(
            L"container run --name {} --network {} --network-alias db {} true", WslcContainerName, TestNetworkName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        const auto networkName = wsl::shared::string::WideToMultiByte(TestNetworkName);
        VERIFY_IS_TRUE(inspect.NetworkSettings.Networks.contains(networkName));
        const auto& endpoint = inspect.NetworkSettings.Networks.at(networkName);
        VERIFY_IS_TRUE(std::ranges::find(endpoint.Aliases, "db") != endpoint.Aliases.end());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_NetworkAlias_NoNetwork_Rejected)
    {
        auto result =
            RunWslc(std::format(L"container run --rm --network-alias db --name {} {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr =
                 L"Network aliases require a user-defined network. Use --network to specify one.\r\nError code: E_INVALIDARG\r\n",
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_NetworkAlias_NoneMode_Rejected)
    {
        auto result = RunWslc(std::format(
            L"container run --rm --network none --network-alias db --name {} {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr =
                 L"Network aliases require a user-defined network. Use --network to specify one.\r\nError code: E_INVALIDARG\r\n",
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_NetworkAlias_MultipleNetworks_Rejected)
    {
        auto result = RunWslc(std::format(
            L"container run --rm --network bridge --network bridge --network-alias db --name {} {} true",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(
            result.StderrContainsSubstring(L"Network aliases cannot be specified when multiple networks are requested. Use a "
                                           L"single --network argument.\r\nError code: E_INVALIDARG"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_NetworkAlias_EmptyValue_Rejected)
    {
        auto result = RunWslc(
            std::format(L"container run --rm --network-alias \"\" --name {} {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(
            result.StderrContainsSubstring(L"Invalid network-alias value: network alias cannot be empty or whitespace"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Volume_NamedVolume_Success)
    {
        // Create a named volume
        auto result = RunWslc(std::format(L"volume create {}", WslcVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Create a container with --rm that uses the named volume and writes a file to it
        result = RunWslc(std::format(
            L"container run --rm --volume {}:/data {} sh -c \"echo -n 'WSLC Named Volume Test' > /data/test.txt\"",
            WslcVolumeName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Create another container that mounts the same named volume and verify the file content
        result = RunWslc(std::format(L"container run --rm --volume {}:/data {} cat /data/test.txt", WslcVolumeName, DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"WSLC Named Volume Test", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Volume_NamedVolume_AutoCreate)
    {
        auto result = RunWslc(std::format(
            L"container run --rm --volume {}:/data {} sh -c \"echo -n 'WSLC Named Volume Test' > /data/test.txt\"",
            WslcVolumeName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Verify the volume was auto-created by removing it (fails if it doesn't exist).
        result = RunWslc(std::format(L"volume rm {}", WslcVolumeName));
        result.Verify({.Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_WithLabel_Success)
    {
        auto result = RunWslc(std::format(
            L"container run --name {} --label A=1 --label B=2 {} echo hello", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"hello\n", .Stderr = L"", .ExitCode = 0});

        auto inspect = InspectContainer(WslcContainerName);
        VERIFY_ARE_EQUAL("1", inspect.Labels["A"]);
        VERIFY_ARE_EQUAL("2", inspect.Labels["B"]);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_StopSignal)
    {
        constexpr int ExpectedExitCode = 42;
        auto result = RunWslc(std::format(
            LR"(container run -d --stop-signal SIGUSR1 --name {} {} bash -c "trap 'exit {}' SIGUSR1; while true; do sleep 1; done")",
            WslcContainerName,
            DebianImage.NameAndTag(),
            ExpectedExitCode));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container stop {}", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        VERIFY_IS_FALSE(inspect.State.Running);
        VERIFY_ARE_EQUAL(ExpectedExitCode, inspect.State.ExitCode);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_StopTimeout)
    {
        // A positive value is forwarded to the container configuration.
        {
            constexpr int ExpectedStopTimeout = 25;
            auto result = RunWslc(std::format(
                L"container run -d --stop-timeout {} --name {} {} sleep infinity",
                ExpectedStopTimeout,
                WslcContainerName,
                DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto inspect = InspectContainer(WslcContainerName);
            VERIFY_IS_TRUE(inspect.Config.StopTimeout.has_value());
            VERIFY_ARE_EQUAL(ExpectedStopTimeout, inspect.Config.StopTimeout.value());
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        // A value of 0 (stop the container immediately) is a valid, explicit timeout.
        {
            auto result = RunWslc(std::format(
                L"container run -d --stop-timeout 0 --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto inspect = InspectContainer(WslcContainerName);
            VERIFY_IS_TRUE(inspect.Config.StopTimeout.has_value());
            VERIFY_ARE_EQUAL(0, inspect.Config.StopTimeout.value());
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        // A value of -1 means "no timeout"; it is a valid, explicit value forwarded to the configuration.
        {
            auto result = RunWslc(std::format(
                L"container run -d --stop-timeout -1 --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto inspect = InspectContainer(WslcContainerName);
            VERIFY_IS_TRUE(inspect.Config.StopTimeout.has_value());
            VERIFY_ARE_EQUAL(-1, inspect.Config.StopTimeout.value());
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        // When --stop-timeout is not specified, no timeout is forwarded to the container configuration.
        {
            auto result =
                RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto inspect = InspectContainer(WslcContainerName);
            VERIFY_IS_FALSE(inspect.Config.StopTimeout.has_value());
            EnsureContainerDoesNotExist(WslcContainerName);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_StopTimeout_Invalid)
    {
        {
            auto result =
                RunWslc(std::format(L"container run --rm --stop-timeout abc --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stdout = L"", .ExitCode = 1});
            VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Invalid stop-timeout argument value: abc"));
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container run --rm --stop-timeout -2 --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid stop timeout value: -2\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        // Validate that the correct error is displayed if the user passes the exact 'WSLC_STOP_TIMEOUT_DEFAULT' value.
        {
            auto result = RunWslc(std::format(
                L"container run --rm --stop-timeout {} --name {} {}", WSLC_STOP_TIMEOUT_DEFAULT, WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"Invalid stop timeout value: -2147483648\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
            EnsureContainerDoesNotExist(WslcContainerName);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_ShmSize)
    {
        auto result = RunWslc(std::format(L"container run --rm --shm-size 128M {} df -h /dev/shm", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
        VERIFY_IS_TRUE(result.Stdout->find(L"128M") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_ShmSize_Invalid)
    {
        {
            auto result =
                RunWslc(std::format(L"container run --rm --shm-size invalid --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stdout = L"", .ExitCode = 1});
            VERIFY_IS_TRUE(result.StderrContainsSubstring(
                L"Invalid shm-size argument value: 'invalid'. Expected a memory size (e.g. 256M, 1G)"));
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container run --rm --shm-size 128X --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stdout = L"", .ExitCode = 1});
            VERIFY_IS_TRUE(result.StderrContainsSubstring(
                L"Invalid shm-size argument value: '128X'. Expected a memory size (e.g. 256M, 1G)"));
            EnsureContainerDoesNotExist(WslcContainerName);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_HealthCheck)
    {
        // All health-check options are forwarded to the container configuration.
        {
            auto result = RunWslc(std::format(
                LR"(container run -d --health-cmd "exit 0" --health-interval 5s --health-timeout 3s --health-retries 2 --health-start-period 1s --name {} {} sleep infinity)",
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
            VERIFY_ARE_EQUAL(5'000'000'000LL, health.Interval.value_or(0));
            VERIFY_ARE_EQUAL(3'000'000'000LL, health.Timeout.value_or(0));
            VERIFY_ARE_EQUAL(1'000'000'000LL, health.StartPeriod.value_or(0));
            VERIFY_ARE_EQUAL(2, health.Retries.value_or(0));
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        // When no health option is specified, no health check is forwarded.
        {
            auto result =
                RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stderr = L"", .ExitCode = 0});

            const auto inspect = InspectContainer(WslcContainerName);
            VERIFY_IS_FALSE(inspect.Config.Healthcheck.has_value());
            EnsureContainerDoesNotExist(WslcContainerName);
        }
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_HealthCheck_Invalid)
    {
        auto result = RunWslc(
            std::format(L"container run --rm --health-timeout invalid --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Invalid health-timeout argument value"));
        EnsureContainerDoesNotExist(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_HealthStatus_Healthy)
    {
        // A health check that always succeeds should drive the container to the "healthy" state.
        auto result = RunWslc(std::format(
            LR"(container run -d --health-cmd "exit 0" --health-interval 1s --health-timeout 3s --health-retries 1 --name {} {} sleep infinity)",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto health = WaitForContainerHealth(WslcContainerName, "healthy");
        VERIFY_ARE_EQUAL(0, health.FailingStreak);
        VERIFY_IS_FALSE(health.Log.empty());
        VERIFY_ARE_EQUAL(0, health.Log.back().ExitCode);

        EnsureContainerDoesNotExist(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_HealthStatus_Unhealthy)
    {
        // A health check that always fails should drive the container to the "unhealthy" state.
        auto result = RunWslc(std::format(
            LR"(container run -d --health-cmd "exit 1" --health-interval 1s --health-timeout 3s --health-retries 1 --name {} {} sleep infinity)",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto health = WaitForContainerHealth(WslcContainerName, "unhealthy");
        VERIFY_IS_TRUE(health.FailingStreak >= 1);
        VERIFY_IS_FALSE(health.Log.empty());
        VERIFY_ARE_EQUAL(1, health.Log.back().ExitCode);

        EnsureContainerDoesNotExist(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_HealthStatus_Timeout)
    {
        auto result = RunWslc(std::format(
            LR"(container run -d --health-cmd "sleep 30" --health-interval 1s --health-timeout 1s --health-retries 1 --name {} {} sleep infinity)",
            WslcContainerName,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto health = WaitForContainerHealth(WslcContainerName, "unhealthy");
        VERIFY_IS_TRUE(health.FailingStreak >= 1);
        VERIFY_IS_FALSE(health.Log.empty());
        VERIFY_ARE_EQUAL(-1, health.Log.back().ExitCode);

        EnsureContainerDoesNotExist(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Cpus)
    {
        auto result = RunWslc(std::format(L"container run --name {} --cpus 1.5 {} true", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        VERIFY_ARE_EQUAL(static_cast<int64_t>(1'500'000'000), inspect.HostConfig.NanoCpus);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Memory)
    {
        auto result = RunWslc(std::format(L"container run --name {} --memory 32M {} true", WslcContainerName, DebianImage.NameAndTag()));
        // Note: stderr is not asserted here because some kernels emit a swap-limit warning
        // ("Your kernel does not support swap limit capabilities...") when a memory limit is set.
        result.Verify({.ExitCode = 0});

        const auto inspect = InspectContainer(WslcContainerName);
        VERIFY_ARE_EQUAL(static_cast<int64_t>(32) * 1024 * 1024, inspect.HostConfig.Memory);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Ulimit)
    {
        auto result = RunWslc(std::format(
            L"container run --name {} --ulimit nofile=1024:2048 --ulimit nproc=512 {} true", WslcContainerName, DebianImage.NameAndTag()));
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Cpus_Invalid)
    {
        auto result = RunWslc(std::format(L"container run --rm --cpus 0 --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            L"Invalid cpus argument value: '0'. Expected a positive number of CPUs (e.g. 0.5, 1, 2)"));
        EnsureContainerDoesNotExist(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Memory_Invalid)
    {
        auto result =
            RunWslc(std::format(L"container run --rm --memory invalid --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(
            result.StderrContainsSubstring(L"Invalid memory argument value: 'invalid'. Expected a memory size (e.g. 256M, 1G)"));
        EnsureContainerDoesNotExist(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Ulimit_Invalid)
    {
        auto result =
            RunWslc(std::format(L"container run --rm --ulimit nofile --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"", .ExitCode = 1});
        VERIFY_IS_TRUE(result.StderrContainsSubstring(
            L"Invalid ulimit argument value: 'nofile'. Expected <name>=<soft>[:<hard>] (use -1 for unlimited)"));
        EnsureContainerDoesNotExist(WslcContainerName);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_StopSignal_Invalid)
    {
        {
            auto result = RunWslc(
                std::format(L"container run --rm --stop-signal SIGINVALID --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stdout = L"", .ExitCode = 1});
            VERIFY_IS_TRUE(
                result.StderrContainsSubstring(L"Invalid stop-signal value: SIGINVALID is not a recognized signal name or number "
                                               L"(Example: SIGKILL, kill, or 9)."));
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container run --rm --stop-signal 0 --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stdout = L"", .ExitCode = 1});
            VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Invalid stop-signal value: 0 is out of valid range (1-31)."));
            EnsureContainerDoesNotExist(WslcContainerName);
        }

        {
            auto result =
                RunWslc(std::format(L"container run --rm --stop-signal 99 --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
            result.Verify({.Stdout = L"", .ExitCode = 1});
            VERIFY_IS_TRUE(result.StderrContainsSubstring(L"Invalid stop-signal value: 99 is out of valid range (1-31)."));
            EnsureContainerDoesNotExist(WslcContainerName);
        }
    }

private:
    // Test container name
    const std::wstring WslcContainerName = L"wslc-test-container";
    const std::wstring WslcContainerName2 = L"wslc-test-container-2";

    // Test environment variables
    const std::wstring HostEnvVariableName = L"WSLC_TEST_HOST_ENV";
    const std::wstring HostEnvVariableName2 = L"WSLC_TEST_HOST_ENV2";
    const std::wstring HostEnvVariableValue = L"wslc-host-env-value";
    const std::wstring HostEnvVariableValue2 = L"wslc-host-env-value2";

    // Test images
    const TestImage& DebianImage = DebianTestImage();
    const TestImage& PythonImage = PythonTestImage();

    // Test environment variable files
    std::filesystem::path EnvTestFile1;
    std::filesystem::path EnvTestFile2;

    // Test ports
    const uint16_t ContainerTestPort = 8080;
    const uint16_t HostTestPort1 = 1234;
    const uint16_t HostTestPort2 = 1235;

    // Test named volume
    const std::wstring WslcVolumeName = L"wslc-test-volume";

    // Test user-defined network
    const std::wstring TestNetworkName = L"wslc-test-network";
};
} // namespace WSLCE2ETests
