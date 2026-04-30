/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIExecutionUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI command execution.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "SessionModel.h"

#include "Command.h"
#include "RootCommand.h"
#include "ContainerCommand.h"
#include "ContainerTasks.h"

using namespace wsl::windows::wslc;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIExecutionUnitTests {
// Helper structure to hold test data
struct CommandLineTestCase
{
    std::wstring commandLine;
    std::wstring expectedCommand;
    bool shouldSucceed;
};

class WSLCCLIExecutionUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIExecutionUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    // Test: Verify EnumVariantMap on DataMap for Context Data
    TEST_METHOD(EnumVariantMap_DataMapValidation)
    {
        // DataMap is an EnumVariantMap, but for command execution context data instead of arguments.
        // It does not have rigid typing like the Args map, so this will verify every Data enum value
        // can be added and retrieved successfully. The arguments unit tests have more complex tests
        // for the EnumVariantMap behavior. This one ensures Data enum values are correct.
        wsl::windows::wslc::execution::DataMap dataMap;

        // Verify all data enum values defined.
        auto allDataTypes = std::vector<Data>{};
        for (int i = 0; i < static_cast<int>(Data::Max); ++i)
        {
            Data dataType = static_cast<Data>(i);

            // Add the data to the DataMap with a test value based on its type.
            // Each data type needs to be added here as each enum may have its own value.
            VERIFY_IS_FALSE(dataMap.Contains(dataType));
            bool handled = false;
            if (dataType == Data::Session)
            {
                // Create a null session for testing - Session requires a COM pointer
                wil::com_ptr<IWSLCSession> nullSession; // Creates null COM pointer
                wsl::windows::wslc::models::Session session{nullSession};
                dataMap.Add<Data::Session>(std::move(session));
                handled = true;
            }
            else if (dataType == Data::Containers)
            {
                std::vector<wsl::windows::wslc::models::ContainerInformation> containers;
                dataMap.Add<Data::Containers>(std::move(containers));
                handled = true;
            }
            else if (dataType == Data::ContainerOptions)
            {
                wsl::windows::wslc::models::ContainerOptions options;
                dataMap.Add<Data::ContainerOptions>(std::move(options));
                handled = true;
            }
            else if (dataType == Data::Images)
            {
                std::vector<wsl::windows::wslc::models::ImageInformation> images;
                dataMap.Add<Data::Images>(std::move(images));
                handled = true;
            }
            else if (dataType == Data::Volumes)
            {
                std::vector<WSLCVolumeInformation> volumes;
                dataMap.Add<Data::Volumes>(std::move(volumes));
                handled = true;
            }

            if (!handled)
            {
                VERIFY_FAIL(L"Unhandled Data type in test");
            }

            allDataTypes.push_back(dataType);
            VERIFY_IS_TRUE(dataMap.Contains(dataType));
        }

        // Verify basic retrieval.
        auto& session = dataMap.Get<Data::Session>();
        VERIFY_IS_NULL(session.Get()); // A null ptr was added.

        auto& containers = dataMap.Get<Data::Containers>();
        VERIFY_ARE_EQUAL(0u, containers.size());

        // Other more complex EnumVariantMap tests are in the Args unit tests.
        // This one will just verify all the data types in the Data Map work as expected.
    }

    // Test: SetContainerOptionsFromArgs sets WorkingDirectory when --workdir is provided
    TEST_METHOD(SetContainerOptionsFromArgs_WithWorkDir_SetsWorkingDirectory)
    {
        CLIExecutionContext context;
        context.Args.Add<ArgType::WorkDir>(std::wstring{L"/app"});

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/app"), options.WorkingDirectory);
    }

    // Test: SetContainerOptionsFromArgs leaves WorkingDirectory empty when --workdir is not provided
    TEST_METHOD(SetContainerOptionsFromArgs_WithoutWorkDir_WorkingDirectoryIsEmpty)
    {
        CLIExecutionContext context;

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_IS_TRUE(options.WorkingDirectory.empty());
    }

    // Test: Full parse of 'exec --workdir "" cont1 cmd' rejects empty working directory
    TEST_METHOD(ExecCommand_ParseWorkDirEmptyValue_ThrowsArgumentException)
    {
        // Invoke ContainerExecCommand parsing directly with the subcommand arguments it accepts.
        auto invocation = CreateInvocationFromCommandLine(L"wslc --workdir \"\" cont1 sh");

        ContainerExecCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(
            command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto&) { return true; });
    }

    // Test: Full parse of 'exec --workdir /path cont1 cmd' sets WorkingDirectory
    TEST_METHOD(ExecCommand_ParseWorkDirLongOption_SetsWorkingDirectory)
    {
        // Invoke ContainerExecCommand parsing directly with the subcommand arguments it accepts.
        auto invocation = CreateInvocationFromCommandLine(L"wslc --workdir /tmp/mydir cont1 sh");

        ContainerExecCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/tmp/mydir"), options.WorkingDirectory);
    }

    // Test: Full parse of 'exec -w /path cont1 cmd' (short alias) sets WorkingDirectory
    TEST_METHOD(ExecCommand_ParseWorkDirShortOption_SetsWorkingDirectory)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc -w /app cont1 sh");

        ContainerExecCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/app"), options.WorkingDirectory);
    }

    // Test: Full parse of 'run --workdir "" image cmd' rejects empty working directory
    TEST_METHOD(RunCommand_ParseWorkDirEmptyValue_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --workdir \"\" ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(
            command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto&) { return true; });
    }

    // Test: Full parse of 'run --workdir /path image cmd' sets WorkingDirectory
    TEST_METHOD(RunCommand_ParseWorkDirLongOption_SetsWorkingDirectory)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --workdir /tmp/mydir ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/tmp/mydir"), options.WorkingDirectory);
    }

    // Test: Full parse of 'run -w /path image cmd' (short alias) sets WorkingDirectory
    TEST_METHOD(RunCommand_ParseWorkDirShortOption_SetsWorkingDirectory)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc -w /app ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/app"), options.WorkingDirectory);
    }

    // Test: Full parse of 'create --workdir "" image cmd' rejects empty working directory
    TEST_METHOD(CreateCommand_ParseWorkDirEmptyValue_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --workdir \"\" ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(
            command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto&) { return true; });
    }

    // Test: Full parse of 'create --workdir /path image cmd' sets WorkingDirectory
    TEST_METHOD(CreateCommand_ParseWorkDirLongOption_SetsWorkingDirectory)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --workdir /tmp/mydir ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/tmp/mydir"), options.WorkingDirectory);
    }

    // Test: Full parse of 'create -w /path image cmd' (short alias) sets WorkingDirectory
    TEST_METHOD(CreateCommand_ParseWorkDirShortOption_SetsWorkingDirectory)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc -w /app ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_ARE_EQUAL(std::string("/app"), options.WorkingDirectory);
    }

    // Test: Full parse of 'run --gpus all image' sets Gpu option
    TEST_METHOD(RunCommand_ParseGpusAll_SetsGpuOption)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --gpus all ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_IS_TRUE(options.Gpu);
    }

    // Test: Full parse of 'run --gpus invalid image' rejects invalid GPU value
    TEST_METHOD(RunCommand_ParseGpusInvalid_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --gpus invalid ubuntu sh");

        ContainerRunCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(
            command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto&) { return true; });
    }

    // Test: Full parse of 'create --gpus all image' sets Gpu option
    TEST_METHOD(CreateCommand_ParseGpusAll_SetsGpuOption)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --gpus all ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);
        command.ValidateArguments(context.Args);

        wsl::windows::wslc::task::SetContainerOptionsFromArgs(context);

        const auto& options = context.Data.Get<Data::ContainerOptions>();
        VERIFY_IS_TRUE(options.Gpu);
    }

    // Test: Full parse of 'create --gpus none image' rejects invalid GPU value
    TEST_METHOD(CreateCommand_ParseGpusInvalid_ThrowsArgumentException)
    {
        auto invocation = CreateInvocationFromCommandLine(L"wslc --gpus none ubuntu sh");

        ContainerCreateCommand command{L""};
        CLIExecutionContext context;
        command.ParseArguments(invocation, context.Args);

        VERIFY_THROWS_SPECIFIC(
            command.ValidateArguments(context.Args), wsl::windows::wslc::ArgumentException, [](const auto&) { return true; });
    }

    // Test: Command Line test parsing all cases defined in CommandLineTestCases.h
    // This test verifies the command line parsing logic used by the CLI and executes the same
    // code as the CLI up to the point of command execution, including parsing and argument validtion.
    // It does not actually verify the execution of the command, just that the correct command is
    // found and the provided command line parsed correctly according to the command's defined arguments,
    // and the argument validation rules are correctly applied. The test cases are defined in
    // CommandLineTestCases.h and cover various valid and invalid command lines.
    TEST_METHOD(CommandLineParsing_AllCases)
    {
        std::vector<CommandLineTestCase> testCases = {
#define COMMAND_LINE_TEST_CASE(cmdLine, expectedCmd, shouldPass) {cmdLine, expectedCmd, shouldPass},
#include "CommandLineTestCases.h"
#undef COMMAND_LINE_TEST_CASE
        };

        // Run all test cases
        for (const auto& testCase : testCases)
        {
            LogComment(L"Testing: " + testCase.commandLine);

            // Pre-pend executable name, which will get stripped off by CommandLineToArgvW
            auto fullCommandLine = L"wslc " + testCase.commandLine;

            // Process the command line as Windows does.
            int argc = 0;
            auto argv = CommandLineToArgvW(fullCommandLine.c_str(), &argc);
            std::vector<std::wstring> args;
            for (int i = 1; i < argc; ++i)
            {
                args.emplace_back(argv[i]);
            }

            // And now process the command line like WSLC does.
            bool succeeded = true;
            try
            {
                Invocation invocation{std::move(args)};
                std::unique_ptr<Command> command = std::make_unique<RootCommand>();
                std::unique_ptr<Command> subCommand = command->FindSubCommand(invocation);
                while (subCommand)
                {
                    command = std::move(subCommand);
                    subCommand = command->FindSubCommand(invocation);
                }

                // Ensure we found the expected command
                VERIFY_ARE_EQUAL(testCase.expectedCommand, command->Name());

                CLIExecutionContext context;

                // Parse and validate and compare to expected results.
                command->ParseArguments(invocation, context.Args);
                command->ValidateArguments(context.Args);
            }
            catch (const CommandException& ce)
            {
                LogComment(L"Command line parsing threw an exception: " + ce.Message());
                succeeded = false;
            }
            catch (...)
            {
                LogComment(L"Command line parsing threw an unexpected exception.");
                succeeded = false;
            }

            VERIFY_ARE_EQUAL(testCase.shouldSucceed, succeeded);
        }
    }
};
} // namespace WSLCCLIExecutionUnitTests