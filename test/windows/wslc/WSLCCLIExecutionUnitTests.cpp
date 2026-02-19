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

#include "Command.h"
#include "RootCommand.h"

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
    WSL_TEST_CLASS(WSLCCLIExecutionUnitTests)

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
            switch (dataType)
            {
            case Data::SessionId:
                dataMap.Add(dataType, std::wstring(L"Session1234"));
                break;
            default:
                VERIFY_FAIL(L"Unhandled Data type in test");
            }

            allDataTypes.push_back(dataType);
            VERIFY_IS_TRUE(dataMap.Contains(dataType));
        }

        // Verify basic retrieval.
        auto sessionId = dataMap.Get<Data::SessionId>();
        VERIFY_ARE_EQUAL(L"Session1234", sessionId);

        // Other more complex EnumVariantMap tests are in the Args unit tests.
        // This one will just verify all the data types in the Data Map work as expected.
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