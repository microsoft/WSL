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
#include "ContainerCommand.h"
#include "ImageCommand.h"
#include "SessionCommand.h"
#include "VolumeCommand.h"
#include "RegistryCommand.h"
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
        // DataMap is an EnumVariantMap
        wsl::windows::wslc::execution::DataMap dataMap;

        // Verify all data enum values defined.

        // Verify basic add
        dataMap.Add<Data::SessionId>(L"Session1234");
        VERIFY_IS_TRUE(dataMap.Contains(Data::SessionId));

        // Verify basic retrieval.
        auto sessionId = dataMap.Get<Data::SessionId>();
        VERIFY_ARE_EQUAL(L"Session1234", sessionId);

        // Other more complex EnumVariantMap tests are in the Args unit tests.
        // This one will just verify all the data types in the Data Map work as expected.
    }

    // Test: Parse various command lines and verify results
    TEST_METHOD(CommandLineParsing_AllCases)
    {
        // Define all test cases inline
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
