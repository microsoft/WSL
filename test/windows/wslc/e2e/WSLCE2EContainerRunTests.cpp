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
        EnsureImageIsDeleted(DebianImage);
        EnsureImageIsDeleted(PythonImage);

        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName.c_str(), nullptr));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName2.c_str(), nullptr));

        // Cleanup Winsock
        WSACleanup();
        return true;
    }

    TEST_METHOD_SETUP(TestMethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);

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
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Container_With_Command)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        auto command = L"echo echo_from_container";
        auto result = RunWslc(std::format(L"container run --name {} {} {}", WslcContainerName, DebianImage.NameAndTag(), command));
        result.Verify({.Stdout = L"echo_from_container\n", .Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(WslcContainerName, L"exited");
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
        result.Verify(
            {.Stderr = L"Environment file 'ENV_FILE_NOT_FOUND' cannot be opened for reading\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
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
        result.Verify({.Stderr = L"Environment variable key 'BAD KEY' cannot contain whitespace\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Interactive_TTY)
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
        result.Verify({.Stderr = L"invalid mount path: 'wslc-tmpfs' mount path must be absolute\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Run_Tmpfs_EmptyDestination_Fails)
    {
        auto result = RunWslc(std::format(L"container run --rm --tmpfs :size=64k {}", DebianImage.NameAndTag()));
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
                << L"  -d,--detach       Run container in detached mode\r\n"
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
};
} // namespace WSLCE2ETests