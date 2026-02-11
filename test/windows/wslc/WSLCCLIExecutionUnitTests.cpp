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

namespace WSLCCLIExecutionUnitTests
{
    // Helper structure to hold test data
    struct CommandLineTestCase
    {
        std::wstring commandLine;
        std::wstring expectedCommand;
        std::wstring expectedSubcommand;
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

        // Test: Parse various command lines and verify results
        TEST_METHOD(CommandLineParsing_AllCases)
        {
            // Define all test cases inline
            std::vector<CommandLineTestCase> testCases = {
                { L"container create --name test ubuntu", L"container", L"create", true },
                { L"container list --all", L"container", L"list", true },
                { L"container run -d ubuntu", L"container", L"run", false },
            };

            // Run all test cases
            for (const auto& testCase : testCases)
            {
                LogComment(L"Testing: " + testCase.commandLine);

                
                // Process the command line as Windows does.
                int argc = 0;
                auto argv = CommandLineToArgvW(testCase.commandLine.c_str(), &argc);
                std::vector<std::wstring> args;
                for (int i = 1; i < argc; ++i)
                {
                    args.emplace_back(argv[i]);
                }

                // And now process the command line like WSLC does.
                try
                {
                    Invocation invocation{ std::move(args) };
                    std::unique_ptr<Command> command = std::make_unique<RootCommand>();
                    std::unique_ptr<Command> subCommand = command->FindSubCommand(invocation);
                    while (subCommand)
                    {
                        command = std::move(subCommand);
                        subCommand = command->FindSubCommand(invocation);
                    }

                    CLIExecutionContext context;
                    command->ParseArguments(invocation, context.Args);
                    command->ValidateArguments(context.Args);
                }
                catch (...)
                {
                    if (testCase.shouldSucceed)
                    {
                        VERIFY_FAIL(L"Expected command to succeed but it failed");
                    }
                    else
                    {
                        LogComment(L"Command failed as expected");
                    }

                    continue;
                }

                // If we reach here it did not fail.
                if (testCase.shouldSucceed)
                {
                    LogComment(L"  Expected: Command=" + testCase.expectedCommand + 
                              L", Subcommand=" + testCase.expectedSubcommand);
                }
                else
                {
                    VERIFY_FAIL(L"Expected command to fail but it succeeded");
                }
            }
        }
    };
} // namespace WSLCCLIExecutionUnitTests
