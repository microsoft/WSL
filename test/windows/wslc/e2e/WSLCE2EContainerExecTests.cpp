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

    TEST_METHOD(WSLCE2E_Container_Exec_HelpCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container exec --help");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_MissingContainerId)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(L"container exec");
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'container-id'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_MissingCommand)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec {}", WslcContainerName));
        result.Verify({.Stdout = GetHelpMessage(), .Stderr = L"Required argument not provided: 'command'\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_ContainerNotFound)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container exec {} echo hello", WslcContainerName));
        result.Verify({.ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_SimpleCommand)
    {
        WSL2_TEST_ONLY();

        // Run a container in background
        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Execute a command
        result = RunWslc(std::format(L"container exec {} echo hello", WslcContainerName));
        result.Verify({.Stdout = L"hello\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_Detach)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Execute with detach flag
        result = RunWslc(std::format(L"container exec -d {} sh -c \"sleep 2 && echo done\"", WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        // Should return immediately without waiting
        // (Verification would require checking that process is still running)
    }

    TEST_METHOD(WSLCE2E_Container_Exec_InteractiveTTY)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -itd --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        const auto& expectedPrompt = VT::InspectAndBuildContainerPrompt(WslcContainerName);

        auto session = RunWslcInteractive(std::format(L"container exec -it {} /bin/bash", WslcContainerName));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        session.ExpectStdout(expectedPrompt);

        session.WriteLine("echo hello");
        session.ExpectCommandEcho("echo hello");
        session.ExpectStdout("hello\r\n");
        session.ExpectStdout(expectedPrompt);

        session.ExitAndVerifyNoErrors();
        auto exitCode = session.Wait();
        VERIFY_ARE_EQUAL(0, exitCode);
    }

    TEST_METHOD(WSLCE2E_Container_Exec_InteractiveNoTTY)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -id --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        auto session = RunWslcInteractive(std::format(L"container exec -i {} cat", WslcContainerName));
        VERIFY_IS_TRUE(session.IsRunning(), L"Container session should be running");

        // Write test data to stdin
        session.WriteLine("test line 1");
        session.WriteLine("test line 2");

        // Close stdin to signal EOF to cat
        session.CloseStdin();

        // Wait for cat to exit with code 0
        auto exitCode = session.Wait(10000);
        VERIFY_ARE_EQUAL(0, exitCode, L"Cat should exit with code 0 after receiving EOF");
        session.VerifyNoErrors();
    }

    TEST_METHOD(WSLCE2E_Container_Exec_EnvOption)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec -e {}=A {} env", HostEnvVariableName, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}=A", HostEnvVariableName)));
    }

    TEST_METHOD(WSLCE2E_Container_Exec_EnvOption_KeyOnly_UsesHostValue)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec -e {} {} env", HostEnvVariableName, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        result.Dump();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}={}", HostEnvVariableName, HostEnvVariableValue)));
    }

    TEST_METHOD(WSLCE2E_Container_Exec_EnvFile)
    {
        WSL2_TEST_ONLY();

        WriteEnvFile(EnvTestFile1, {"WSLC_TEST_EXEC_ENV_FILE_A=exec-env-file-a", "WSLC_TEST_EXEC_ENV_FILE_B=exec-env-file-b"});

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec --env-file {} {} env", EscapePath(EnvTestFile1.wstring()), WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_FILE_A=exec-env-file-a"));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_FILE_B=exec-env-file-b"));
    }

    TEST_METHOD(WSLCE2E_Container_Exec_EnvOption_MultipleValues)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(
            L"container exec -e {}=value-a -e {}=value-b {} env", HostEnvVariableName, HostEnvVariableName2, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}=value-a", HostEnvVariableName)));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}=value-b", HostEnvVariableName2)));
    }

    TEST_METHOD(WSLCE2E_Container_Exec_EnvOption_KeyOnly_MultipleValues_UsesHostValues)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec -e {} -e {} {} env", HostEnvVariableName, HostEnvVariableName2, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}={}", HostEnvVariableName, HostEnvVariableValue)));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}={}", HostEnvVariableName2, HostEnvVariableValue2)));
    }

    TEST_METHOD(WSLCE2E_Container_Exec_EnvOption_EmptyValue)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Pass an explicit empty value and verify it is present as KEY=
        result = RunWslc(std::format(L"container exec -e {}= {} env", HostEnvVariableName, WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, std::format(L"{}=", HostEnvVariableName)));
    }

    TEST_METHOD(WSLCE2E_Container_Exec_EnvOption_MixedWithEnvFile)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(WSLCE2E_Container_Exec_EnvFile_MissingFile)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec --env-file ENV_FILE_NOT_FOUND {} env", WslcContainerName));
        result.Verify(
            {.Stderr = L"Environment file 'ENV_FILE_NOT_FOUND' cannot be opened for reading\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_EnvFile_MultipleFiles)
    {
        WSL2_TEST_ONLY();

        WriteEnvFile(EnvTestFile1, {"WSLC_TEST_EXEC_ENV_FILE_MULTI_A=file1-a", "WSLC_TEST_EXEC_ENV_FILE_MULTI_B=file1-b"});
        WriteEnvFile(EnvTestFile2, {"WSLC_TEST_EXEC_ENV_FILE_MULTI_C=file2-c", "WSLC_TEST_EXEC_ENV_FILE_MULTI_D=file2-d"});

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(
            L"container exec --env-file {} --env-file {} {} env", EscapePath(EnvTestFile1.wstring()), EscapePath(EnvTestFile2.wstring()), WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_FILE_MULTI_A=file1-a"));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_FILE_MULTI_B=file1-b"));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_FILE_MULTI_C=file2-c"));
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_FILE_MULTI_D=file2-d"));
    }

    TEST_METHOD(WSLCE2E_Container_Exec_EnvFile_InvalidContent)
    {
        WSL2_TEST_ONLY();

        WriteEnvFile(EnvTestFile1, {"WSLC_TEST_EXEC_ENV_VALID=ok", "BAD KEY=value"});

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec --env-file {} {} env", EscapePath(EnvTestFile1.wstring()), WslcContainerName));
        result.Verify({.Stderr = L"Environment variable key 'BAD KEY' cannot contain whitespace\r\nError code: E_INVALIDARG\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_EnvFile_DuplicateKeys_Precedence)
    {
        WSL2_TEST_ONLY();

        WriteEnvFile(EnvTestFile1, {"WSLC_TEST_EXEC_ENV_DUP=from-file-1"});
        WriteEnvFile(EnvTestFile2, {"WSLC_TEST_EXEC_ENV_DUP=from-file-2"});

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        // Later --env-file wins
        result = RunWslc(std::format(
            L"container exec --env-file {} --env-file {} {} env", EscapePath(EnvTestFile1.wstring()), EscapePath(EnvTestFile2.wstring()), WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_DUP=from-file-2"));

        // Explicit -e wins over env-file
        result = RunWslc(std::format(
            L"container exec -e WSLC_TEST_EXEC_ENV_DUP=from-cli --env-file {} --env-file {} {} env",
            EscapePath(EnvTestFile1.wstring()),
            EscapePath(EnvTestFile2.wstring()),
            WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_DUP=from-cli"));
    }

    TEST_METHOD(WSLCE2E_Container_Exec_EnvFile_ValueContainsEquals)
    {
        WSL2_TEST_ONLY();

        WriteEnvFile(EnvTestFile1, {"WSLC_TEST_EXEC_ENV_EQUALS=value=with=equals"});

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        result = RunWslc(std::format(L"container exec --env-file {} {} env", EscapePath(EnvTestFile1.wstring()), WslcContainerName));
        result.Verify({.Stderr = L"", .ExitCode = S_OK});

        const auto outputLines = result.GetStdoutLines();
        VERIFY_IS_TRUE(ContainsOutputLine(outputLines, L"WSLC_TEST_EXEC_ENV_EQUALS=value=with=equals"));
    }

    TEST_METHOD(WSLCE2E_Container_Exec_ExitCode_Propagates)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec {} sh -c \"exit 42\"", WslcContainerName));
        result.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 42});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_Stderr_Propagates)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec {} sh -c \"echo exec-error 1>&2\"", WslcContainerName));
        result.Verify({.Stdout = L"", .Stderr = L"exec-error\n", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_StoppedContainer)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run --name {} {} echo hello", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec {} echo should-fail", WslcContainerName));
        result.Verify({.Stderr = L"The group or resource is not in the correct state to perform the requested operation. \r\nError code: ERROR_INVALID_STATE\r\n", .ExitCode = 1});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_WorkDir)
    {
        WSL2_TEST_ONLY();

        auto result = RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        result.Verify({.Stderr = L"", .ExitCode = 0});

        result = RunWslc(std::format(L"container exec --workdir /tmp {} pwd", WslcContainerName));
        result.Verify({.Stdout = L"/tmp\n", .Stderr = L"", .ExitCode = 0});
    }

    TEST_METHOD(WSLCE2E_Container_Exec_WorkDir_ShortAlias)
    {
        WSL2_TEST_ONLY();

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
                << L"  -w,--workdir      Working directory inside the container\r\n"
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
};
} // namespace WSLCE2ETests
