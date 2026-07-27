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
    WSLC_TEST_CLASS(WSLCCLIParserUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    TEST_METHOD(ParserTest_ParserCases)
    {
        std::vector<ParserTestCase> testCases = {
#define WSLC_PARSER_TEST_CASE(argSetValue, expected, cmdLine) {ArgumentSet::argSetValue, expected, cmdLine},
            WSLC_PARSER_TEST_CASES
#undef WSLC_PARSER_TEST_CASE
        };

        for (const auto& testCase : testCases)
        {
            bool succeeded = false;
            const bool optionsOnly = IsOptionsOnlySet(testCase.argumentSet);

            try
            {
                Log::Comment(String().Format(L"Testing: %ls", testCase.commandLine.c_str()));
                auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(testCase.commandLine);

                std::vector<Argument> definedArgs = GetArgumentsForSet(testCase.argumentSet);

                ArgMap args;
                ParseArgumentsStateMachine stateMachine{inv, args, std::move(definedArgs), optionsOnly};
                while (stateMachine.Step())
                {
                    stateMachine.ThrowIfError();
                }
                // Step() returns false on stop in optionsOnly mode without surfacing any
                // pending error, so drain once more to convert "missing value at EOF"
                // into a thrown ArgumentException.
                stateMachine.ThrowIfError();

                if (!args.Contains(ArgType::Help))
                {
                    for (const auto& arg : GetArgumentsForSet(testCase.argumentSet))
                    {
                        // Required-arg enforcement is a *whole-command-line* concern.
                        // In optionsOnly mode the parser only saw part of the input, so
                        // missing values would be reported by the second (subcommand)
                        // pass, not this one. Skip required checks for those sets.
                        if (!optionsOnly && arg.Required() && !args.Contains(arg.Type()))
                        {
                            throw ArgumentException(std::wstring(L"Required argument missing: ") + arg.Name());
                        }

                        if (args.Contains(arg.Type()))
                        {
                            arg.Validate(args);
                        }
                    }
                }

                succeeded = true;

                if (testCase.commandLine.find(L"image1") != std::wstring::npos && testCase.argumentSet == ArgumentSet::Run)
                {
                    VERIFY_IS_TRUE(args.Contains(ArgType::ImageId));
                    auto imageId = args.Get<ArgType::ImageId>();
                    VERIFY_ARE_EQUAL(L"image1", imageId);
                }

                if (testCase.commandLine.find(L"cont1") != std::wstring::npos && testCase.argumentSet == ArgumentSet::List)
                {
                    VERIFY_IS_TRUE(args.Contains(ArgType::ContainerId));
                    auto containerId = args.Get<ArgType::ContainerId>();
                    VERIFY_ARE_EQUAL(L"cont1", containerId);
                }

                if (testCase.commandLine.find(L"--rm") != std::wstring::npos)
                {
                    VERIFY_IS_TRUE(args.Contains(ArgType::Remove));
                }

                if (testCase.commandLine.find(L"command") != std::wstring::npos && testCase.argumentSet == ArgumentSet::Run)
                {
                    VERIFY_IS_TRUE(args.Contains(ArgType::Command));
                    auto command = args.Get<ArgType::Command>();
                    VERIFY_IS_TRUE(command.find(L"command") != std::wstring::npos);
                }

                if (testCase.commandLine.find(L"forward") != std::wstring::npos && testCase.argumentSet == ArgumentSet::Run)
                {
                    VERIFY_IS_TRUE(args.Contains(ArgType::ForwardArgs));
                    auto forwardArgs = args.Get<ArgType::ForwardArgs>();
                    std::wstring forwardArgsConcat = wsl::shared::string::Join(forwardArgs, L' ');
                    VERIFY_IS_TRUE(forwardArgsConcat.find(L"hello world") != std::wstring::npos); // Forward args should contain hello world
                    VERIFY_IS_TRUE(forwardArgsConcat.find(L"image1") == std::wstring::npos); // Forward args should not contain the imageId
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

                // Globals-specific spot checks: the synthetic global flag
                // (--quiet / -q) and value (--session, see GetArgumentsForSet)
                // must have been consumed by the global parser when present,
                // and the stop position must point at the first non-global
                // token (or end()).
                if (testCase.argumentSet == ArgumentSet::Globals)
                {
                    if (testCase.commandLine.find(L"--quiet") != std::wstring::npos || testCase.commandLine.find(L"-q") != std::wstring::npos)
                    {
                        VERIFY_IS_TRUE(args.Contains(ArgType::Quiet));
                    }

                    if (testCase.commandLine.find(L"--session") != std::wstring::npos)
                    {
                        VERIFY_IS_TRUE(args.Contains(ArgType::Session));
                        VERIFY_ARE_EQUAL(std::wstring(L"foo"), args.Get<ArgType::Session>());
                    }
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

            VERIFY_ARE_EQUAL(testCase.expectedResult, succeeded, String().Format(L"Command line: %ls", testCase.commandLine.c_str()));
        }
    }

    // Options-only mode: parser consumes leading options and stops at the first
    // positional without consuming it. inv must advance past consumed options.
    TEST_METHOD(OptionsOnly_StopsAtFirstPositional)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --verbose image1 command");

        std::vector<Argument> defs = {
            Argument::Create(ArgType::Verbose),
            Argument::Create(ArgType::NoColor),
        };

        ArgMap args;
        ParseArgumentsStateMachine sm{inv, args, std::move(defs), /*optionsOnly*/ true};
        while (sm.Step())
        {
            sm.ThrowIfError();
        }

        VERIFY_IS_TRUE(args.Contains(ArgType::Verbose));
        VERIFY_IS_FALSE(args.Contains(ArgType::NoColor));

        // Position points at the first positional ("image1") so the next pass
        // can resume from there via Invocation::consumeUntil.
        auto pos = sm.Position();
        VERIFY_IS_TRUE(pos != inv.end());
        VERIFY_ARE_EQUAL(std::wstring(L"image1"), *pos);
    }

    TEST_METHOD(OptionsOnly_NoOptionsPresent_StopsImmediately)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc image1");

        std::vector<Argument> defs = {Argument::Create(ArgType::Verbose)};

        ArgMap args;
        ParseArgumentsStateMachine sm{inv, args, std::move(defs), /*optionsOnly*/ true};
        while (sm.Step())
        {
            sm.ThrowIfError();
        }

        VERIFY_IS_FALSE(args.Contains(ArgType::Verbose));
        VERIFY_ARE_EQUAL(std::wstring(L"image1"), *sm.Position());
    }

    TEST_METHOD(OptionsOnly_OnlyOptions_PositionEqualsEnd)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --verbose");

        std::vector<Argument> defs = {Argument::Create(ArgType::Verbose)};

        ArgMap args;
        ParseArgumentsStateMachine sm{inv, args, std::move(defs), /*optionsOnly*/ true};
        while (sm.Step())
        {
            sm.ThrowIfError();
        }

        VERIFY_IS_TRUE(args.Contains(ArgType::Verbose));
        VERIFY_IS_TRUE(sm.Position() == inv.end());
    }

    TEST_METHOD(OptionsOnly_AdjoinedValue_IsConsumed)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --signal=9 image1");

        std::vector<Argument> defs = {Argument::Create(ArgType::Signal)};

        ArgMap args;
        ParseArgumentsStateMachine sm{inv, args, std::move(defs), /*optionsOnly*/ true};
        while (sm.Step())
        {
            sm.ThrowIfError();
        }

        VERIFY_IS_TRUE(args.Contains(ArgType::Signal));
        VERIFY_ARE_EQUAL(std::wstring(L"9"), args.Get<ArgType::Signal>());
        VERIFY_ARE_EQUAL(std::wstring(L"image1"), *sm.Position());
    }

    TEST_METHOD(OptionsOnly_SeparatedValue_IsConsumed)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --signal 9 image1");

        std::vector<Argument> defs = {Argument::Create(ArgType::Signal)};

        ArgMap args;
        ParseArgumentsStateMachine sm{inv, args, std::move(defs), /*optionsOnly*/ true};
        while (sm.Step())
        {
            sm.ThrowIfError();
        }

        VERIFY_IS_TRUE(args.Contains(ArgType::Signal));
        VERIFY_ARE_EQUAL(std::wstring(L"9"), args.Get<ArgType::Signal>());
        VERIFY_ARE_EQUAL(std::wstring(L"image1"), *sm.Position());
    }

    TEST_METHOD(OptionsOnly_UnknownOption_Throws)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --doesnotexist image1");

        std::vector<Argument> defs = {Argument::Create(ArgType::Verbose)};

        ArgMap args;
        bool threw = false;
        try
        {
            ParseArgumentsStateMachine sm{inv, args, std::move(defs), /*optionsOnly*/ true};
            while (sm.Step())
            {
                sm.ThrowIfError();
            }
        }
        catch (const ArgumentException&)
        {
            threw = true;
        }

        VERIFY_IS_TRUE(threw);
    }

    TEST_METHOD(OptionsOnly_ValueAtEndOfInput_Throws)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --signal");

        std::vector<Argument> defs = {Argument::Create(ArgType::Signal)};

        ArgMap args;
        bool threw = false;
        try
        {
            ParseArgumentsStateMachine sm{inv, args, std::move(defs), /*optionsOnly*/ true};
            while (sm.Step())
            {
                sm.ThrowIfError();
            }
            sm.ThrowIfError();
        }
        catch (const ArgumentException&)
        {
            threw = true;
        }

        VERIFY_IS_TRUE(threw);
    }

    // After options-only stops, Invocation::consumeUntil(Position()) hands the
    // remaining tokens to a second parse pass — exactly what Main.cpp does.
    TEST_METHOD(OptionsOnly_TwoPassParseAcrossPositional)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --verbose image1 --signal 9");

        // Pass 1: options-only with just Verbose visible.
        std::vector<Argument> globalDefs = {Argument::Create(ArgType::Verbose)};
        ArgMap globals;
        ParseArgumentsStateMachine sm1{inv, globals, std::move(globalDefs), /*optionsOnly*/ true};
        while (sm1.Step())
        {
            sm1.ThrowIfError();
        }
        inv.consumeUntil(sm1.Position());

        VERIFY_IS_TRUE(globals.Contains(ArgType::Verbose));
        VERIFY_IS_FALSE(globals.Contains(ArgType::Signal));

        // Pass 2: full mode with the subcommand's argument set.
        std::vector<Argument> subDefs = {
            Argument::Create(ArgType::ImageId, true),
            Argument::Create(ArgType::Signal),
        };
        ArgMap subArgs;
        ParseArgumentsStateMachine sm2{inv, subArgs, std::move(subDefs), /*optionsOnly*/ false};
        while (sm2.Step())
        {
            sm2.ThrowIfError();
        }

        VERIFY_IS_TRUE(subArgs.Contains(ArgType::ImageId));
        VERIFY_ARE_EQUAL(std::wstring(L"image1"), subArgs.Get<ArgType::ImageId>());
        VERIFY_IS_TRUE(subArgs.Contains(ArgType::Signal));
        VERIFY_ARE_EQUAL(std::wstring(L"9"), subArgs.Get<ArgType::Signal>());
    }

    // stopOnUnknown: unknown -alias / --name / lone '-' / bare '--' tokens
    // back the iterator up and stop cleanly instead of throwing. Recognized
    // options before the unknown one are still consumed.
    TEST_METHOD(OptionsOnly_StopOnUnknown_LeavesTokenForCaller)
    {
        // Case 1: leading unknown --name. Nothing consumed, position == begin.
        {
            auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --doesnotexist image1");
            std::vector<Argument> defs = {Argument::Create(ArgType::Verbose)};

            ArgMap args;
            ParseArgumentsStateMachine sm{inv, args, std::move(defs), /*optionsOnly*/ true, /*stopOnUnknown*/ true};
            while (sm.Step())
            {
                sm.ThrowIfError();
            }
            sm.ThrowIfError();

            VERIFY_IS_FALSE(args.Contains(ArgType::Verbose));
            VERIFY_ARE_EQUAL(std::wstring(L"--doesnotexist"), *sm.Position());
        }

        // Case 2: leading unknown -alias. Same backing-up behavior.
        {
            auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc -X image1");
            std::vector<Argument> defs = {Argument::Create(ArgType::Verbose)};

            ArgMap args;
            ParseArgumentsStateMachine sm{inv, args, std::move(defs), /*optionsOnly*/ true, /*stopOnUnknown*/ true};
            while (sm.Step())
            {
                sm.ThrowIfError();
            }
            sm.ThrowIfError();

            VERIFY_IS_FALSE(args.Contains(ArgType::Verbose));
            VERIFY_ARE_EQUAL(std::wstring(L"-X"), *sm.Position());
        }

        // Case 3: recognized option consumed, then unknown stops the scan.
        {
            auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --verbose --doesnotexist image1");
            std::vector<Argument> defs = {Argument::Create(ArgType::Verbose)};

            ArgMap args;
            ParseArgumentsStateMachine sm{inv, args, std::move(defs), /*optionsOnly*/ true, /*stopOnUnknown*/ true};
            while (sm.Step())
            {
                sm.ThrowIfError();
            }
            sm.ThrowIfError();

            VERIFY_IS_TRUE(args.Contains(ArgType::Verbose));
            VERIFY_ARE_EQUAL(std::wstring(L"--doesnotexist"), *sm.Position());
        }

        // Case 4: lone '-' with no positionals defined backs up instead of erroring.
        {
            auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc -");
            std::vector<Argument> defs = {Argument::Create(ArgType::Verbose)};

            ArgMap args;
            ParseArgumentsStateMachine sm{inv, args, std::move(defs), /*optionsOnly*/ true, /*stopOnUnknown*/ true};
            while (sm.Step())
            {
                sm.ThrowIfError();
            }
            sm.ThrowIfError();

            VERIFY_ARE_EQUAL(std::wstring(L"-"), *sm.Position());
        }
    }

    // Preloaded env-style default can be replaced by a single CLI occurrence
    // even though the arg's Limit is 1.
    TEST_METHOD(OverridableDefaults_CliValueReplacesPreload)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --signal 9");

        std::vector<Argument> defs = {Argument::Create(ArgType::Signal)};

        ArgMap args;
        args.Add(ArgType::Signal, std::wstring(L"15")); // pretend env preloaded SIGTERM

        ParseArgumentsStateMachine sm{inv, args, defs, /*optionsOnly*/ false, /*stopOnUnknown*/ false, /*overridableDefaults*/ defs};
        while (sm.Step())
        {
            sm.ThrowIfError();
        }
        sm.ThrowIfError();

        VERIFY_ARE_EQUAL(1u, args.Count(ArgType::Signal));
        VERIFY_ARE_EQUAL(std::wstring(L"9"), args.Get<ArgType::Signal>());
    }

    // A preloaded (env-style) default followed by multiple CLI values collapses to the
    // final CLI value: the preload is dropped and single-value args are last-wins.
    TEST_METHOD(PreloadedDefault_LastCliValueWins)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --signal 9 --signal 1");

        std::vector<Argument> defs = {Argument::Create(ArgType::Signal)};

        ArgMap args;
        args.Add(ArgType::Signal, std::wstring(L"15"));

        ParseArgumentsStateMachine sm{inv, args, defs, /*optionsOnly*/ false, /*stopOnUnknown*/ false, /*overridableDefaults*/ defs};
        while (sm.Step())
        {
            sm.ThrowIfError();
        }
        sm.ThrowIfError();

        VERIFY_ARE_EQUAL(1u, args.Count(ArgType::Signal));
        VERIFY_ARE_EQUAL(std::wstring(L"1"), args.Get<ArgType::Signal>());
    }

    // Preloaded flag default plus CLI mention of the same flag stays a single
    // entry (current flag value is always 'true').
    TEST_METHOD(OverridableDefaults_FlagPreloadCoexistsWithCli)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --verbose");

        std::vector<Argument> defs = {Argument::Create(ArgType::Verbose)};

        ArgMap args;
        args.Add(ArgType::Verbose, true); // pretend env preloaded it

        ParseArgumentsStateMachine sm{inv, args, defs, /*optionsOnly*/ false, /*stopOnUnknown*/ false, /*overridableDefaults*/ defs};
        while (sm.Step())
        {
            sm.ThrowIfError();
        }
        sm.ThrowIfError();

        VERIFY_ARE_EQUAL(1u, args.Count(ArgType::Verbose));
        VERIFY_IS_TRUE(args.Get<ArgType::Verbose>());
    }

    // Duplicate flag on the CLI (no env preload) folds to one entry: docker-style.
    TEST_METHOD(DuplicateFlagOnCli_IsIdempotent)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --verbose --verbose");

        std::vector<Argument> defs = {Argument::Create(ArgType::Verbose)};

        ArgMap args;
        ParseArgumentsStateMachine sm{inv, args, defs};
        while (sm.Step())
        {
            sm.ThrowIfError();
        }
        sm.ThrowIfError();

        VERIFY_ARE_EQUAL(1u, args.Count(ArgType::Verbose));
        VERIFY_IS_TRUE(args.Get<ArgType::Verbose>());
    }

    // Duplicate single-value arg on the CLI (no preload) is last-wins (docker-style):
    // the final value replaces the earlier one instead of accumulating.
    TEST_METHOD(DuplicateValueOnCli_LastWins)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --signal 9 --signal 1");

        std::vector<Argument> defs = {Argument::Create(ArgType::Signal)};

        ArgMap args;
        ParseArgumentsStateMachine sm{inv, args, defs};
        while (sm.Step())
        {
            sm.ThrowIfError();
        }
        sm.ThrowIfError();

        VERIFY_ARE_EQUAL(1u, args.Count(ArgType::Signal));
        VERIFY_ARE_EQUAL(std::wstring(L"1"), args.Get<ArgType::Signal>());
    }

    // Unlimited value args are exempt from last-wins: every CLI occurrence accumulates.
    TEST_METHOD(UnlimitedValueOnCli_Accumulates)
    {
        ArgMap args = ParseFlags(L"wslc --publish 80:80 --publish 443:443", {Argument::Create(ArgType::Publish, false, Limit::Unlimited)});

        VERIFY_ARE_EQUAL(2u, args.Count(ArgType::Publish));
    }

    // Boolean flags store their explicit parsed value: present with true or false when the
    // flag is specified, absent when it is not. Consumers read them via ArgMap::GetFlag,
    // which returns the stored value if present or a caller-supplied default if absent. The
    // helper parses a single command line against the supplied defs and returns the resulting
    // ArgMap so each case can assert the stored flag value.
    static ArgMap ParseFlags(const std::wstring& commandLine, std::vector<Argument> defs)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(commandLine);

        ArgMap args;
        ParseArgumentsStateMachine sm{inv, args, std::move(defs)};
        while (sm.Step())
        {
            sm.ThrowIfError();
        }
        sm.ThrowIfError();
        return args;
    }

    // "--flag" and every recognized true form store a single true entry. The single-letter
    // "t"/"T" forms are Docker-parity extensions enabled for the CLI flag path.
    TEST_METHOD(Flag_TrueForms_StoreSingleTrueEntry)
    {
        for (const auto* cmd :
             {L"wslc --verbose",
              L"wslc --verbose=true",
              L"wslc --verbose=1",
              L"wslc --verbose=TRUE",
              L"wslc --verbose=t",
              L"wslc --verbose=T"})
        {
            Log::Comment(String().Format(L"Testing: %ls", cmd));
            ArgMap args = ParseFlags(cmd, {Argument::Create(ArgType::Verbose)});

            VERIFY_IS_TRUE(args.Contains(ArgType::Verbose));
            VERIFY_ARE_EQUAL(1u, args.Count(ArgType::Verbose));
            VERIFY_IS_TRUE(args.Get<ArgType::Verbose>());
            VERIFY_IS_TRUE(args.GetFlag<ArgType::Verbose>());
        }
    }

    // Every recognized false form stores the flag present with value false (a docker-style
    // "--flag=false"), so Contains() is true but GetFlag() reports false. The single-letter
    // "f"/"F" forms are Docker-parity extensions enabled for the CLI flag path.
    TEST_METHOD(Flag_FalseForms_StoreSingleFalseEntry)
    {
        for (const auto* cmd :
             {L"wslc --verbose=false", L"wslc --verbose=0", L"wslc --verbose=False", L"wslc --verbose=f", L"wslc --verbose=F"})
        {
            Log::Comment(String().Format(L"Testing: %ls", cmd));
            ArgMap args = ParseFlags(cmd, {Argument::Create(ArgType::Verbose)});

            VERIFY_IS_TRUE(args.Contains(ArgType::Verbose));
            VERIFY_ARE_EQUAL(1u, args.Count(ArgType::Verbose));
            VERIFY_IS_FALSE(args.Get<ArgType::Verbose>());
            VERIFY_IS_FALSE(args.GetFlag<ArgType::Verbose>());
        }
    }

    // A non-boolean adjoined value is an error rather than being silently ignored.
    TEST_METHOD(Flag_InvalidBoolean_Throws)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --verbose=maybe");

        std::vector<Argument> defs = {Argument::Create(ArgType::Verbose)};

        ArgMap args;
        ParseArgumentsStateMachine sm{inv, args, std::move(defs)};

        bool threw = false;
        try
        {
            while (sm.Step())
            {
                sm.ThrowIfError();
            }
            sm.ThrowIfError();
        }
        catch (const ArgumentException&)
        {
            threw = true;
        }

        VERIFY_IS_TRUE(threw);
    }

    // Docker parity: a space-separated token after a boolean flag is NOT consumed as the
    // flag's value; the flag is true and the token becomes the next positional.
    TEST_METHOD(Flag_SpaceSeparatedValue_StaysPositional)
    {
        ArgMap args = ParseFlags(
            L"wslc --verbose true", {Argument::Create(ArgType::Verbose), Argument::Create(ArgType::ContainerId, false, Limit::Unlimited)});

        VERIFY_IS_TRUE(args.Contains(ArgType::Verbose));
        VERIFY_IS_TRUE(args.Get<ArgType::Verbose>());
        VERIFY_ARE_EQUAL(1u, args.Count(ArgType::ContainerId));
        VERIFY_ARE_EQUAL(std::wstring(L"true"), args.Get<ArgType::ContainerId>());
    }

    // Alias forms honor adjoined booleans just like the long name.
    TEST_METHOD(Flag_AliasAdjoinedBoolean)
    {
        VERIFY_IS_TRUE(ParseFlags(L"wslc -q", {Argument::Create(ArgType::Quiet)}).GetFlag<ArgType::Quiet>());
        VERIFY_IS_TRUE(ParseFlags(L"wslc -q=true", {Argument::Create(ArgType::Quiet)}).GetFlag<ArgType::Quiet>());
        VERIFY_IS_FALSE(ParseFlags(L"wslc -q=false", {Argument::Create(ArgType::Quiet)}).GetFlag<ArgType::Quiet>());
    }

    // Docker-parity single-letter forms ("t"/"T"/"f"/"F") are honored on the alias form too.
    TEST_METHOD(Flag_AliasShortBooleanForms)
    {
        VERIFY_IS_TRUE(ParseFlags(L"wslc -q=t", {Argument::Create(ArgType::Quiet)}).GetFlag<ArgType::Quiet>());
        VERIFY_IS_TRUE(ParseFlags(L"wslc -q=T", {Argument::Create(ArgType::Quiet)}).GetFlag<ArgType::Quiet>());
        VERIFY_IS_FALSE(ParseFlags(L"wslc -q=f", {Argument::Create(ArgType::Quiet)}).GetFlag<ArgType::Quiet>());
        VERIFY_IS_FALSE(ParseFlags(L"wslc -q=F", {Argument::Create(ArgType::Quiet)}).GetFlag<ArgType::Quiet>());
    }

    // An adjoined boolean value may be wrapped in double quotes (e.g. --flag="true"), just like
    // an adjoined value argument. The quotes are stripped before the boolean is parsed, on both
    // the named and alias forms.
    TEST_METHOD(Flag_QuotedAdjoinedBoolean)
    {
        VERIFY_IS_TRUE(ParseFlags(L"wslc --verbose=\"true\"", {Argument::Create(ArgType::Verbose)}).GetFlag<ArgType::Verbose>());
        VERIFY_IS_FALSE(ParseFlags(L"wslc --verbose=\"false\"", {Argument::Create(ArgType::Verbose)}).GetFlag<ArgType::Verbose>());
        VERIFY_IS_TRUE(ParseFlags(L"wslc -q=\"true\"", {Argument::Create(ArgType::Quiet)}).GetFlag<ArgType::Quiet>());
        VERIFY_IS_FALSE(ParseFlags(L"wslc -q=\"false\"", {Argument::Create(ArgType::Quiet)}).GetFlag<ArgType::Quiet>());
        VERIFY_IS_TRUE(ParseFlags(L"wslc --verbose=\"t\"", {Argument::Create(ArgType::Verbose)}).GetFlag<ArgType::Verbose>());
        VERIFY_IS_FALSE(ParseFlags(L"wslc --verbose=\"f\"", {Argument::Create(ArgType::Verbose)}).GetFlag<ArgType::Verbose>());
    }

    // In an alias chain, leading flags are true and a trailing "=false" turns only the
    // last flag off.
    TEST_METHOD(Flag_AliasChain_TrailingFalse)
    {
        std::vector<Argument> defs = {Argument::Create(ArgType::Quiet), Argument::Create(ArgType::Interactive)};

        ArgMap all = ParseFlags(L"wslc -qi", defs);
        VERIFY_IS_TRUE(all.GetFlag<ArgType::Quiet>());
        VERIFY_IS_TRUE(all.GetFlag<ArgType::Interactive>());

        ArgMap trailingFalse = ParseFlags(L"wslc -qi=false", defs);
        VERIFY_IS_TRUE(trailingFalse.GetFlag<ArgType::Quiet>());
        VERIFY_IS_FALSE(trailingFalse.GetFlag<ArgType::Interactive>());
    }

    // Repeated flags are last-wins (matching docker) and never accumulate multiple entries:
    // "--flag --flag=false" ends up false, the reverse ends up true. The flag is stored either
    // way (a single entry), so GetFlag reports the winning value.
    TEST_METHOD(Flag_Repeated_LastWins)
    {
        ArgMap trueThenFalse = ParseFlags(L"wslc --verbose --verbose=false", {Argument::Create(ArgType::Verbose)});
        VERIFY_IS_FALSE(trueThenFalse.GetFlag<ArgType::Verbose>());
        VERIFY_ARE_EQUAL(1u, trueThenFalse.Count(ArgType::Verbose));

        ArgMap falseThenTrue = ParseFlags(L"wslc --verbose=false --verbose", {Argument::Create(ArgType::Verbose)});
        VERIFY_IS_TRUE(falseThenTrue.GetFlag<ArgType::Verbose>());
        VERIFY_ARE_EQUAL(1u, falseThenTrue.Count(ArgType::Verbose));

        ArgMap duplicateTrue = ParseFlags(L"wslc --verbose --verbose=true", {Argument::Create(ArgType::Verbose)});
        VERIFY_ARE_EQUAL(1u, duplicateTrue.Count(ArgType::Verbose));
    }

    // "--flag=false" overrides a preloaded (env-style) default of true, replacing it with a
    // single stored false rather than leaving a lingering true. GetFlag then reports false.
    TEST_METHOD(Flag_FalseOverridesPreloadedDefault)
    {
        auto inv = WSLCTestHelpers::CreateInvocationFromCommandLine(L"wslc --verbose=false");

        std::vector<Argument> defs = {Argument::Create(ArgType::Verbose)};

        ArgMap args;
        args.Add(ArgType::Verbose, true); // pretend env preloaded it to true

        ParseArgumentsStateMachine sm{inv, args, defs, /*optionsOnly*/ false, /*stopOnUnknown*/ false, /*overridableDefaults*/ defs};
        while (sm.Step())
        {
            sm.ThrowIfError();
        }
        sm.ThrowIfError();

        VERIFY_IS_TRUE(args.Contains(ArgType::Verbose));
        VERIFY_ARE_EQUAL(1u, args.Count(ArgType::Verbose));
        VERIFY_IS_FALSE(args.GetFlag<ArgType::Verbose>());
    }

    // A flag whose behavior is on by default is read with GetFlag(true): absent yields the
    // default (true), "--flag=false" yields false, and "--flag" yields true. A bare Contains()
    // cannot express this: it reports true for both "--flag" and "--flag=false" and false when
    // the flag is absent, so it distinguishes neither the two stored values nor absent-as-default.
    TEST_METHOD(Flag_GetFlagDefaultTrue_DefaultOnFlag)
    {
        std::vector<Argument> defs = {Argument::Create(ArgType::Remove)};

        ArgMap absent = ParseFlags(L"wslc", defs);
        VERIFY_IS_FALSE(absent.Contains(ArgType::Remove));
        VERIFY_IS_TRUE(absent.GetFlag<ArgType::Remove>(true));

        ArgMap disabled = ParseFlags(L"wslc --rm=false", defs);
        VERIFY_IS_TRUE(disabled.Contains(ArgType::Remove));
        VERIFY_IS_FALSE(disabled.GetFlag<ArgType::Remove>(true));

        ArgMap enabled = ParseFlags(L"wslc --rm", defs);
        VERIFY_IS_TRUE(enabled.GetFlag<ArgType::Remove>(true));
    }
};

} // namespace WSLCCLIParserUnitTests
