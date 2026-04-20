/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCE2EContainerExecTests.cpp

Abstract:

    This file contains end-to-end tests for WSLC container exec command.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {

class WSLCE2EContainerExecTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerExecTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName.c_str(), HostEnvVariableValue.c_str()));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(HostEnvVariableName2.c_str(), HostEnvVariableValue2.c_str()));
        VERIFY_IS_TRUE(::SetEnvironmentVariableW(MissingHostEnvVariableName.c_str(), nullptr));

        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
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
        EnsureContainerDoesNotExist(WslcContainerName);
        return true;
    }

    TEST_METHOD_CLEANUP(TestMethodCleanup)
    {
        DeleteFileW(EnvTestFile1.c_str());
        DeleteFileW(EnvTestFile2.c_str());
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_HelpCommand)
    {
        auto result = RunWslc(L"container exec --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_MissingContainerId)
    {
        auto result = RunWslc(L"container exec");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'container-id'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_MissingCommand)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec {}", WslcContainerName));
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'command'\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_ContainerNotFound)
    {
        auto result = RunWslc(std::format(L"container exec {} echo hello", WslcContainerName));
        result.Verify(
            {.Stderr = std::format(L"Container '{}' not found.\r\nError code: WSLC_E_CONTAINER_NOT_FOUND\r\n", WslcContainerName),
             .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_SimpleCommand)
    {
        // Run a container in background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Execute a command
        result = RunWslc(std::format(L"container exec {} echo hello", WslcContainerName));
        result.Verify({.Stdout = L"hello\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_InteractiveTTY)
    {
        VerifyContainerIsNotListed(WslcContainerName);

        const auto& prompt = ">";
        auto result =
            RunWslc(std::format(L"container run -itd -e PS1={} --name {} {}", prompt, WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_InteractiveNoTTY)
    {
        VerifyContainerIsNotListed(WslcContainerName);
        auto result = RunWslc(std::format(L"container run -id --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});
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

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvOption)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec -e {}=A {} env", HostEnvVariableName, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}=A", HostEnvVariableName)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvOption_KeyOnly_UsesHostValue)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec -e {} {} env", HostEnvVariableName, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}={}", HostEnvVariableName, HostEnvVariableValue)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvFile)
    {
        WriteTestFile(EnvTestFile1, {"WSLC_TEST_EXEC_ENV_FILE_A=exec-env-file-a", "WSLC_TEST_EXEC_ENV_FILE_B=exec-env-file-b"});

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec --env-file {} {} env", EscapePath(EnvTestFile1.wstring()), WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_EXEC_ENV_FILE_A=exec-env-file-a"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_EXEC_ENV_FILE_B=exec-env-file-b"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvOption_MultipleValues)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(
            L"container exec -e {}=value-a -e {}=value-b {} env", HostEnvVariableName, HostEnvVariableName2, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}=value-a", HostEnvVariableName)));
        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}=value-b", HostEnvVariableName2)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvOption_KeyOnly_MultipleValues_UsesHostValues)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec -e {} -e {} {} env", HostEnvVariableName, HostEnvVariableName2, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}={}", HostEnvVariableName, HostEnvVariableValue)));
        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}={}", HostEnvVariableName2, HostEnvVariableValue2)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvOption_EmptyValue)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Pass an explicit empty value and verify it is present as KEY=
        result = RunWslc(std::format(L"container exec -e {}= {} env", HostEnvVariableName, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(std::format(L"{}=", HostEnvVariableName)));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvOption_MixedWithEnvFile)
    {
        WriteTestFile(EnvTestFile1, {"WSLC_TEST_EXEC_ENV_MIX_FILE_A=from-file-a", "WSLC_TEST_EXEC_ENV_MIX_FILE_B=from-file-b"});

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(
            L"container exec -e WSLC_TEST_EXEC_ENV_MIX_CLI=from-cli --env-file {} {} env", EscapePath(EnvTestFile1.wstring()), WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_EXEC_ENV_MIX_FILE_A=from-file-a"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_EXEC_ENV_MIX_FILE_B=from-file-b"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_EXEC_ENV_MIX_CLI=from-cli"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvFile_MissingFile)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec --env-file ENV_FILE_NOT_FOUND {} env", WslcContainerName));
        result.Verify(
            {.Stderr = L"Environment file 'ENV_FILE_NOT_FOUND' cannot be opened for reading\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvFile_MultipleFiles)
    {
        WriteTestFile(EnvTestFile1, {"WSLC_TEST_EXEC_ENV_FILE_MULTI_A=file1-a", "WSLC_TEST_EXEC_ENV_FILE_MULTI_B=file1-b"});
        WriteTestFile(EnvTestFile2, {"WSLC_TEST_EXEC_ENV_FILE_MULTI_C=file2-c", "WSLC_TEST_EXEC_ENV_FILE_MULTI_D=file2-d"});

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(
            L"container exec --env-file {} --env-file {} {} env", EscapePath(EnvTestFile1.wstring()), EscapePath(EnvTestFile2.wstring()), WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_EXEC_ENV_FILE_MULTI_A=file1-a"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_EXEC_ENV_FILE_MULTI_B=file1-b"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_EXEC_ENV_FILE_MULTI_C=file2-c"));
        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_EXEC_ENV_FILE_MULTI_D=file2-d"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvFile_InvalidContent)
    {
        WriteTestFile(EnvTestFile1, {"WSLC_TEST_EXEC_ENV_VALID=ok", "BAD KEY=value"});

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec --env-file {} {} env", EscapePath(EnvTestFile1.wstring()), WslcContainerName));
        result.Verify({.Stderr = L"Environment variable key 'BAD KEY' cannot contain whitespace\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvFile_DuplicateKeys_Precedence)
    {
        WriteTestFile(EnvTestFile1, {"WSLC_TEST_EXEC_ENV_DUP=from-file-1"});
        WriteTestFile(EnvTestFile2, {"WSLC_TEST_EXEC_ENV_DUP=from-file-2"});

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Later --env-file wins
        result = RunWslc(std::format(
            L"container exec --env-file {} --env-file {} {} env", EscapePath(EnvTestFile1.wstring()), EscapePath(EnvTestFile2.wstring()), WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_EXEC_ENV_DUP=from-file-2"));

        // Explicit -e wins over env-file
        result = RunWslc(std::format(
            L"container exec -e WSLC_TEST_EXEC_ENV_DUP=from-cli --env-file {} --env-file {} {} env",
            EscapePath(EnvTestFile1.wstring()),
            EscapePath(EnvTestFile2.wstring()),
            WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_EXEC_ENV_DUP=from-cli"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_EnvFile_ValueContainsEquals)
    {
        WriteTestFile(EnvTestFile1, {"WSLC_TEST_EXEC_ENV_EQUALS=value=with=equals"});

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec --env-file {} {} env", EscapePath(EnvTestFile1.wstring()), WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        VERIFY_IS_TRUE(result.StdoutContainsLine(L"WSLC_TEST_EXEC_ENV_EQUALS=value=with=equals"));
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_ExitCode_Propagates)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec {} sh -c \"exit 42\"", WslcContainerName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 42});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_Stderr_Propagates)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec {} sh -c \"echo exec-error 1>&2\"", WslcContainerName));
        result.Verify({.Stdout = L"", .Stderr = L"exec-error\n", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_StoppedContainer)
    {
        auto result = RunWslc(std::format(L"container run --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto inspect = InspectContainer(WslcContainerName);
        result = RunWslc(std::format(L"container exec {} echo should-fail", WslcContainerName));
        auto errorMessage = std::format(L"Container '{}' is not running.\r\nError code: WSLC_E_CONTAINER_NOT_RUNNING\r\n", inspect.Id);
        result.Verify({.Stderr = errorMessage, .ExitCode = 1});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_UserOption_UidRoot)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec -u 0 {} sh -c \"id -u; id -g\"", WslcContainerName));
        result.Verify({.Stdout = L"0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_UserOption_NameGroupRoot)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec -u root:root {} sh -c \"id -un; id -u; id -g\"", WslcContainerName));
        result.Verify({.Stdout = L"root\n0\n0\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_UserOption_InvalidGroup_Fails)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec -u root:badgid {} id -u", WslcContainerName));
        result.Verify({.Stdout = L"unable to find group badgid: no matching entries in group file\r\n", .ExitCode = 126});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_WorkDir)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec --workdir /tmp {} pwd", WslcContainerName));
        result.Verify({.Stdout = L"/tmp\n", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Exec_WorkDir_ShortAlias)
    {
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec -w /tmp {} pwd", WslcContainerName));
        result.Verify({.Stdout = L"/tmp\n", .Stderr = L"", .ExitCode = 0});
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container";
    const TestImage& DebianImage = DebianTestImage();

    // Test environment variables
    const std::wstring HostEnvVariableName = L"WSLC_TEST_HOST_ENV";
    const std::wstring HostEnvVariableName2 = L"WSLC_TEST_HOST_ENV2";
    const std::wstring HostEnvVariableValue = L"wslc-host-env-value";
    const std::wstring HostEnvVariableValue2 = L"wslc-host-env-value2";
    const std::wstring MissingHostEnvVariableName = L"WSLC_TEST_MISSING_HOST_ENV";

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
        return L"Executes a command in a running container.\r\n\r\n";
    }

    std::wstring GetUsage() const
    {
        return L"Usage: wslc container exec [<options>] <container-id> <command> [<arguments>...]\r\n\r\n";
    }

    std::wstring GetAvailableCommands() const
    {
        std::wstringstream commands;
        commands << L"The following arguments are available:\r\n"
                 << L"  container-id      Container ID\r\n"
                 << L"  command           The command to run\r\n"
                 << L"  arguments         Arguments to pass to the command being executed inside the container\r\n"
                 << L"\r\n";
        return commands.str();
    }

    std::wstring GetAvailableOptions() const
    {
        std::wstringstream options;
        options << L"The following options are available:\r\n"
                << L"  -d,--detach       Run container in detached mode\r\n"
                << L"  -e,--env          Key=Value pairs for environment variables\r\n"
                << L"  --env-file        File containing key=value pairs of env variables\r\n"
                << L"  -i,--interactive  Attach to stdin and keep it open\r\n"
                << L"  --session         Specify the session to use\r\n"
                << L"  -t,--tty          Open a TTY with the container process.\r\n"
                << L"  -u,--user         User ID for the process (name|uid|uid:gid)\r\n"
                << L"  -w,--workdir      Working directory inside the container\r\n"
                << L"  -h,--help         Shows help about the selected command\r\n"
                << L"\r\n";
        return options.str();
    }
};
} // namespace WSLCE2ETests
