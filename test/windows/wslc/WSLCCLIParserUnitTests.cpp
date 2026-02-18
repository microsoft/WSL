/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIParserUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI argument parsing and validation.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "Argument.h"
#include "ArgumentTypes.h"
#include "ArgumentParser.h"
#include "Invocation.h"
#include "ParserTestCases.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::argument;

using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIParserUnitTests {

class WSLCCLIParserUnitTests
{
    WSL_TEST_CLASS(WSLCCLIParserUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    // Test: Verify command line to argv mapping and GetRemainingRawCommandLineFromIndex
    TEST_METHOD(ParserTest_StateMachine_PositionalForward)
    {
        // Build test cases from x-macro
        std::vector<ParserTestCase> testCases = {
#define WSLC_PARSER_TEST_CASE(argSetValue, expected, cmdLine) \
            { ArgumentSet::argSetValue, expected, cmdLine },
            WSLC_PARSER_TEST_CASES
#undef WSLC_PARSER_TEST_CASE
        };

        for (const auto& testCase : testCases)
        {
            try
            {
                Log::Comment(String().Format(L"Testing: %ls", testCase.commandLine.c_str()));
                auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(testCase.commandLine);

                // Get argument definitions from the helper function
                std::vector<Argument> definedArgs = GetArgumentsForSet(testCase.argumentSet);

                ArgMap args;
                ParseArgumentsStateMachine stateMachine{inv, args, std::move(definedArgs)};
                while (stateMachine.Step())
                {
                    stateMachine.ThrowIfError();
                }

                if (testCase.commandLine.find(L"cont1") != std::wstring::npos)
                {
                    VERIFY_IS_TRUE(args.Contains(ArgType::ContainerId));
                    auto containerId = args.Get<ArgType::ContainerId>();
                    VERIFY_ARE_EQUAL(L"cont1", containerId);
                }

                if (testCase.commandLine.find(L"rm") != std::wstring::npos)
                {
                    // Ensure 'rm' was parsed wherever it was found.
                    VERIFY_IS_TRUE(args.Contains(ArgType::Remove));
                }

                if (testCase.commandLine.find(L"command") != std::wstring::npos)
                {
                    VERIFY_IS_TRUE(args.Contains(ArgType::Command));
                    auto command = args.Get<ArgType::Command>();
                    VERIFY_IS_TRUE(command.find(L"command") != std::wstring::npos);
                }

                if (testCase.commandLine.find(L"forward") != std::wstring::npos)
                {
                    VERIFY_IS_TRUE(args.Contains(ArgType::ForwardArgs));
                    auto forwardArgs = args.Get<ArgType::ForwardArgs>();
                    std::wstring forwardArgsConcat;
                    for (const auto& arg : forwardArgs)
                    {
                        if (!forwardArgsConcat.empty())
                        {
                            forwardArgsConcat += L" ";
                        }
                        forwardArgsConcat += arg;
                    }
                    VERIFY_IS_TRUE(forwardArgsConcat.find(L"hello world") != std::wstring::npos); // Forward args should contain hello world
                    VERIFY_IS_TRUE(forwardArgsConcat.find(L"cont1") == std::wstring::npos); // Forward args should not contain the containerId
                    VERIFY_IS_TRUE(forwardArgsConcat.find(L"command") == std::wstring::npos); // Forward args should not contain the command
                    LogComment(L"Forwarded Args: " + forwardArgsConcat);
                }

                if (testCase.commandLine.find(L"443") != std::wstring::npos)
                {
                    VERIFY_IS_TRUE(args.Contains(ArgType::Publish));
                    auto publishArgs = args.GetAll<ArgType::Publish>();
                    VERIFY_ARE_EQUAL(2, publishArgs.size());              // Should have both publish args
                    VERIFY_ARE_NOT_EQUAL(publishArgs[0], publishArgs[1]); // Both publish args should be different
                }
            }
            catch (ArgumentException& ex)
            {
                if (testCase.expectedResult)
                {
                    VERIFY_FAIL(String().Format(L"Test case threw unexpected argument exception: %ls", ex.Message().c_str()));
                }
                else
                {
                    Log::Comment(String().Format(L"Test case threw expected argument exception: %ls", ex.Message().c_str()));
                }
            }
            catch (std::exception& ex)
            {
                if (testCase.expectedResult)
                {
                    VERIFY_FAIL(String().Format(L"Test case threw unexpected exception: %hs", ex.what()));
                }
                else
                {
                    Log::Comment(String().Format(L"Test case threw expected exception: %hs", ex.what()));
                }
            }
        }
    }
};
} // namespace WSLCCLIParserUnitTests