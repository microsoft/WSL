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

        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName.c_str(), HostEnvVariableValue.c_str()));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName2.c_str(), HostEnvVariableValue2.c_str()));
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);

        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName.c_str(), nullptr));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName2.c_str(), nullptr));
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

    TEST_METHOD(WSLCE2E_Container_Run_Entrypoint)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run --rm --entrypoint /bin/whoami {}", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"root\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Run_Entrypoint_And_Arguments)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(
            std::format(L"container run --rm --entrypoint /bin/echo {} hello from entrypoint with args", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"hello from entrypoint with args\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Run_Entrypoint_Invalid_Path)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run --rm --entrypoint /bin/does-not-exist {}", DebianImage.NameAndTag()));
        result.Verify(
            {.Stdout = L"", .Stderr = L"failed to create task for container: failed to create shim task: OCI runtime create failed: runc create failed: unable to start container process: error during container init: exec: \"/bin/does-not-exist\": stat /bin/does-not-exist: no such file or directory: unknown\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Run_Entrypoint_Detach_Lifecycle)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(
            L"container run --name {} -d --entrypoint /bin/sleep {} infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VerifyContainerIsListed(WslcContainerName, L"running");
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
        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}=A", HostEnvVariableName)));
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

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}=A", HostEnvVariableName)));
        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}=B", HostEnvVariableName2)));
    }

    TEST_METHOD(WSLCE2E_Container_Run_EnvOption_KeyOnly_UsesHostValue)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {} {} env", WslcContainerName, HostEnvVariableName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}={}", HostEnvVariableName, HostEnvVariableValue)));
    }

    TEST_METHOD(WSLCE2E_Container_Run_EnvOption_KeyOnly_MultipleValues_UsesHostValues)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {} -e {} {} env",
            WslcContainerName,
            HostEnvVariableName,
            HostEnvVariableName2,
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}={}", HostEnvVariableName, HostEnvVariableValue)));
        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}={}", HostEnvVariableName2, HostEnvVariableValue2)));
    }

    TEST_METHOD(WSLCE2E_Container_Run_EnvOption_EmptyValue)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e {}= {} env", WslcContainerName, HostEnvVariableName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}=", HostEnvVariableName)));
    }

    TEST_METHOD(WSLCE2E_Container_Run_EnvFile)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        WriteFile(EnvTestFile1, {"WSLC_TEST_ENV_FILE_A=env-file-a", "WSLC_TEST_ENV_FILE_B=env-file-b"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_FILE_A=env-file-a"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_FILE_B=env-file-b"));
    }

    TEST_METHOD(WSLCE2E_Container_Run_EnvOption_MixedWithEnvFile)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        WriteFile(EnvTestFile1, {"WSLC_TEST_ENV_MIX_FILE_A=from-file-a", "WSLC_TEST_ENV_MIX_FILE_B=from-file-b"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} -e WSLC_TEST_ENV_MIX_CLI=from-cli --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_MIX_FILE_A=from-file-a"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_MIX_FILE_B=from-file-b"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_MIX_CLI=from-cli"));
    }

    TEST_METHOD(WSLCE2E_Container_Run_EnvFile_MultipleFiles)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        WriteFile(EnvTestFile1, {"WSLC_TEST_ENV_FILE_MULTI_A=file1-a", "WSLC_TEST_ENV_FILE_MULTI_B=file1-b"});

        WriteFile(EnvTestFile2, {"WSLC_TEST_ENV_FILE_MULTI_C=file2-c", "WSLC_TEST_ENV_FILE_MULTI_D=file2-d"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            EscapePath(EnvTestFile2.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_FILE_MULTI_A=file1-a"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_FILE_MULTI_B=file1-b"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_FILE_MULTI_C=file2-c"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_FILE_MULTI_D=file2-d"));
    }

    TEST_METHOD(WSLCE2E_Container_Run_EnvFile_MissingFile)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file ENV_FILE_NOT_FOUND {} env", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr = L"Environment file 'ENV_FILE_NOT_FOUND' cannot be opened for reading\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Run_EnvFile_InvalidContent)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        WriteFile(EnvTestFile1, {"WSLC_TEST_ENV_VALID=ok", "BAD KEY=value"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"Environment variable key 'BAD KEY' cannot contain whitespace\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Run_EnvFile_DuplicateKeys_Precedence)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        WriteFile(EnvTestFile1, {"WSLC_TEST_ENV_DUP=from-file-1"});

        WriteFile(EnvTestFile2, {"WSLC_TEST_ENV_DUP=from-file-2"});

        // Later --env-file should win over earlier --env-file for duplicate keys
        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            EscapePath(EnvTestFile2.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_DUP=from-file-2"));

        // Explicit -e should win over env-file value for duplicate keys
        result = RunWslc(std::format(
            L"container run --rm --name {} -e WSLC_TEST_ENV_DUP=from-cli --env-file {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            EscapePath(EnvTestFile2.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_DUP=from-cli"));
    }

    TEST_METHOD(WSLCE2E_Container_Run_EnvFile_ValueContainsEquals)
    {
        WSL2_TEST_ONLY();
        VerifyContainerIsNotListed(WslcContainerName);

        WriteFile(EnvTestFile1, {"WSLC_TEST_ENV_EQUALS=value=with=equals"});

        auto result = RunWslc(std::format(
            L"container run --rm --name {} --env-file {} {} env",
            WslcContainerName,
            EscapePath(EnvTestFile1.wstring()),
            DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_ENV_EQUALS=value=with=equals"));
    }

    TEST_METHOD(WSLCE2E_Container_Run_UserOption_NameRoot)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run --rm -u root {} sh -c \"id -un; id -u; id -g\"", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"root\n0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Run_UserOption_UidRoot)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run --rm -u 0 {} id -u", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"0\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Run_UserOption_UidGidRoot)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run --rm -u 0:0 {} sh -c \"id -u; id -g\"", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Run_UserOption_UnknownUser_Fails)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run --rm -u user_does_not_exist {} id -u", DebianImage.NameAndTag()));
        result.Verify(
            {.Stderr = L"unable to find user user_does_not_exist: no matching entries in passwd file\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Run_UserOption_UnknownGroup_Fails)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run --rm -u root:badgid {} id -u", DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"unable to find group badgid: no matching entries in group file\r\nError code: E_FAIL\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Run_UserOption_NameGroupRoot)
    {
        WSL2_TEST_ONLY();

        auto result =
            RunWslc(std::format(L"container run --rm -u root:root {} sh -c \"id -un; id -u; id -g\"", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"root\n0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Run_UserOption_NonRootUser_Succeeds)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run --rm -u nobody {} sh -c \"id -un; id -u; id -g\"", DebianImage.NameAndTag()));
        result.Verify({.Stdout = L"nobody\n65534\n65534\n", .Stderr = L"", .ExitCode = 0});
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

    // Test environment variable files
    std::filesystem::path EnvTestFile1;
    std::filesystem::path EnvTestFile2;

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
                << L"  -t,--tty          Open a TTY with the container process.\r\n"
                << L"  -u,--user         User ID for the process (name|uid|uid:gid)\r\n"
                << L"  -v,--volume       Bind mount a volume to the container\r\n"
                << L"  -h,--help         Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests