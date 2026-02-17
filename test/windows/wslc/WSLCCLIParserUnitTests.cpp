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

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::argument;

using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIParserUnitTests {

// Note that for raw string literals we must use double backslashes for a single backslash.
static const std::wstring s_parserTestCases[] = {
    // Basic usage cases of wslc.
    LR"(wslc)",
    LR"(wslc diag list --verbose)",
    LR"(wslc run --name "my container" ubuntu)",
    LR"(wslc exec container -- echo "hello world")",
    LR"(wslc run --env PATH=/bin:/usr/bin container)",
    LR"(wslc --help)",
    LR"(wslc run --name test --env VAR=value -- cmd /c echo test)",
    LR"(wslc run "c:\\program files\\app.exe")",
    LR"(wslc run --arg "value with \\"quotes\\"")",
    LR"(wslc run --flag1 --flag2 --flag3)",
    LR"(wslc diag)",

    // Ensure we are correctly handling quoted path executable.
    LR"("C:\\Program Files\\My App\\app.exe")",
    LR"("C:\\Program Files\\My App\\app.exe" diag list --verbose)",
    LR"("C:\\Program Files\\My App\\app.exe" run "c:\\program files\\app.exe")",

    // Special parser cases to validate we are properly handling the command line parsing of Windows:
    // https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments?view=msvc-170
    LR"(exe "a b c" d e)",
    LR"(exe "ab\\"c" "\\\\" d)",
    LR"(exe a\\\\\\b d"e f"g h)",
    LR"(exe a\\\\"b c d)",
    LR"(exe a\\\\\\"b c" d e)",

    // This is a special case that executes the known quirk in CommandLineToArgvW where a quote preceded by
    // an odd number of backslashes is treated as a literal quote instead of a string delimiter. This is
    // a case that will fail if the parser follows exact documented CommandLineToArgvW behavior. Documented
    // behavior results in a single argument, as the quote would be treated as a literal character and not
    // end quote mode, but the actual behavior of CommandLineToArgvW is that the quote is treated as a string
    // delimiter even when preceded by an odd number of backslashes. Parsing expectation is that it would be
    // one argument. Actual behavior is that it is 3 arguments. We must match actual behavior!
    LR"(exe a"b"" c d)",
};

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

    // Test: Verify boundary conditions
    TEST_METHOD(ParserTest_InvocationBoundaryConditions)
    {
        const wchar_t* cmdLine = L"wslc a b";
        int argc = 0;
        wil::unique_hlocal_ptr<LPWSTR[]> argv;
        argv.reset(CommandLineToArgvW(cmdLine, &argc));
        std::vector<std::wstring> args;
        for (int i = 1; i < argc; ++i)
        {
            args.push_back(argv[i]);
        }

        size_t argsSize = args.size();
        Invocation inv(std::move(args), cmdLine);
        auto first = inv.GetRemainingRawCommandLineFromIndex(0);
        VERIFY_IS_FALSE(first.empty());
        auto last = inv.GetRemainingRawCommandLineFromIndex(argsSize - 1);
        VERIFY_IS_FALSE(last.empty());
        auto beyond = inv.GetRemainingRawCommandLineFromIndex(argsSize);
        VERIFY_IS_TRUE(beyond.empty());
        auto wayBeyond = inv.GetRemainingRawCommandLineFromIndex(9999);
        VERIFY_IS_TRUE(wayBeyond.empty());
    }

    // Test: Verify command line to argv mapping and GetRemainingRawCommandLineFromIndex
    TEST_METHOD(ParserTest_InvocationCommandLineMapping)
    {
        for (const auto& testCase : s_parserTestCases)
        {
            Log::Comment(String().Format(L"Testing: %ls", testCase.c_str()));
            auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(testCase);

            // Track all remaining command lines we see
            std::vector<std::wstring> remainingCommandLines;
            std::vector<size_t> commandLineLengths;

            // Iterate through each argument
            size_t expectedIndex = 0;
            for (auto it = inv.begin(); it != inv.end(); ++it, ++expectedIndex)
            {
                size_t currentIndex = it.index();
                VERIFY_ARE_EQUAL(currentIndex, expectedIndex);

                // Get remaining command line from this index
                auto remaining = inv.GetRemainingRawCommandLineFromIndex(currentIndex);
                Log::Comment(String().Format(
                    L"  [%zu] arg='%ls', remaining='%.*ls'",
                    currentIndex,
                    it->c_str(),
                    static_cast<int>(remaining.length()),
                    remaining.data()));
                std::wstring remainingStr(remaining);
                remainingCommandLines.push_back(remainingStr);
                commandLineLengths.push_back(remaining.length());

                // Verify: Each remaining command line should be smaller than the previous.
                if (currentIndex > 0)
                {
                    VERIFY_IS_LESS_THAN(
                        remaining.length(),
                        commandLineLengths[currentIndex - 1],
                        String().Format(
                            L"Remaining at index %zu (%zu chars) should be < index %zu (%zu chars)",
                            currentIndex,
                            remaining.length(),
                            currentIndex - 1,
                            commandLineLengths[currentIndex - 1]));
                }

                // Verify: The remaining command line should contain the current argument
                if (!remaining.empty())
                {
                    std::wstring currentArg = *it;
                    bool found = false;

                    // Check various forms the argument might appear in the raw command line
                    // (quoted, unquoted, escaped, etc.)

                    // Check if argument appears quoted
                    std::wstring quotedArg = L"\"" + currentArg + L"\"";
                    if (remaining.find(quotedArg) != std::wstring::npos)
                    {
                        found = true;
                    }
                    // Check if argument appears unquoted at start
                    else if (remaining.find(currentArg) == 0)
                    {
                        found = true;
                    }
                    // Check if it appears after the start (with space before)
                    else if (remaining.find(currentArg) != std::wstring::npos)
                    {
                        found = true;
                    }
                    // For arguments with special characters, just verify remaining is non-empty
                    else if (currentArg.find_first_of(L"\\\"= ") != std::wstring::npos)
                    {
                        found = !remaining.empty();
                    }

                    VERIFY_IS_TRUE(
                        found,
                        String().Format(
                            L"Remaining should relate to current argument: '%s' in '%.*s'",
                            currentArg.c_str(),
                            static_cast<int>(std::min(remaining.length(), 100ull)),
                            remaining.data()));
                }
            }

            // Verify: Number of unique remaining command lines equals number of arguments
            std::set<std::wstring> uniqueRemaining(remainingCommandLines.begin(), remainingCommandLines.end());

            Log::Comment(String().Format(L"  Total args: %zu", inv.size()));
            Log::Comment(String().Format(L"  Unique remaining command lines: %zu", uniqueRemaining.size()));

            VERIFY_ARE_EQUAL(
                uniqueRemaining.size(),
                inv.size(),
                String().Format(L"Should have %zu unique remaining command lines for %zu arguments", inv.size(), inv.size()));

            // Verify: Each remaining command line should be unique
            for (size_t i = 1; i < remainingCommandLines.size(); ++i)
            {
                VERIFY_ARE_NOT_EQUAL(
                    remainingCommandLines[i],
                    remainingCommandLines[i - 1],
                    String().Format(L"Remaining command lines at indices %zu and %zu should be different", i - 1, i));
            }

            Log::Comment(L"  [OK] Test case passed\n");
        }
    }

    // Test: Verify command line to argv mapping and GetRemainingRawCommandLineFromIndex
    TEST_METHOD(ParserTest_StateMachine_PositionalForward)
    {
        // These test cases assume we are at root command to test only argument parsing.
        // All cases should succeed.
        std::vector<std::wstring> stateMachineTestCases = {
            // Simple case with required arg and simple other args.
            LR"(wslc -?)",
            LR"(wslc cont1)",
            LR"(wslc --verbose cont1)",

            // Value tests, flag and non-flag, multi-value.
            LR"(wslc --publish=80:80 cont1)",     // adjoined
            LR"(wslc --publish 80:80 cont1)",     // non-adjoined
            LR"(wslc -p=80:80 cont1)",            // adjoined
            LR"(wslc -p 80:80 cont1)",            // non-adjoined
            LR"(wslc -p 80:80 -p 443:443 cont1)", // multiple values
            LR"(wslc -p=80:80 -p=443:443 cont1)", // multiple values

            // Flag parse tests
            LR"(wslc -v cont1)",
            LR"(wslc -vi cont1)",
            LR"(wslc -rm cont1)",   // 2-char alias only
            LR"(wslc -virm cont1)", // 2-char alias at end
            LR"(wslc -vrmi cont1)", // 2-char alias in between
            LR"(wslc -rmiv cont1)", // 2-char alias in beginning

            // Forward args tests.
            LR"(wslc cont1 forward hello world)",
            LR"(wslc cont1 forward"hello world")",
            LR"(wslc cont1 f="hello world" forward echo)",
        };

        for (const auto& testCase : stateMachineTestCases)
        {
            try
            {
                Log::Comment(String().Format(L"Testing: %ls", testCase.c_str()));
                auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(testCase);

                std::vector<Argument> definedArgs = {
                    Argument::Create(ArgType::ContainerId, true), // Required positional argument
                    Argument::Create(ArgType::ForwardArgs, false),
                    Argument::Create(ArgType::Help),
                    Argument::Create(ArgType::Interactive),
                    Argument::Create(ArgType::Verbose),
                    Argument::Create(ArgType::Remove),
                    Argument::Create(ArgType::Publish, false, 3), // Not required, up to 3 values.
                };

                ArgMap args;
                ParseArgumentsStateMachine stateMachine{inv, args, std::move(definedArgs)};
                while (stateMachine.Step())
                {
                    stateMachine.ThrowIfError();
                }

                if (testCase.find(L"cont1") != std::wstring::npos)
                {
                    VERIFY_IS_TRUE(args.Contains(ArgType::ContainerId));
                    auto containerId = args.Get<ArgType::ContainerId>();
                    VERIFY_ARE_EQUAL(L"cont1", containerId);
                }

                if (testCase.find(L"rm") != std::wstring::npos)
                {
                    // Ensure 'rm' was parsed wherever it was found.
                    VERIFY_IS_TRUE(args.Contains(ArgType::Remove));
                }

                if (testCase.find(L"forward") != std::wstring::npos)
                {
                    VERIFY_IS_TRUE(args.Contains(ArgType::ForwardArgs));
                    auto forwardArgs = args.Get<ArgType::ForwardArgs>();
                    VERIFY_IS_TRUE(forwardArgs.find(L"hello world") != std::wstring::npos); // Forward args should contain hello world
                    VERIFY_IS_TRUE(forwardArgs.find(L"cont1") == std::wstring::npos); // Forward args should not contain the containerId
                    LogComment(L"Forwarded Args: " + forwardArgs);
                }

                if (testCase.find(L"443") != std::wstring::npos)
                {
                    VERIFY_IS_TRUE(args.Contains(ArgType::Publish));
                    auto publishArgs = args.GetAll<ArgType::Publish>();
                    VERIFY_ARE_EQUAL(2, publishArgs.size());              // Should have both publish args
                    VERIFY_ARE_NOT_EQUAL(publishArgs[0], publishArgs[1]); // Both publish args should be different
                }
            }
            catch (ArgumentException& ex)
            {
                VERIFY_FAIL(String().Format(L"Test case threw argument exception: %ls", ex.Message().c_str()));
            }
            catch (std::exception& ex)
            {
                VERIFY_FAIL(String().Format(L"Test case threw unexpected exception: %hs", ex.what()));
            }
        }
    }
};
} // namespace WSLCCLIParserUnitTests