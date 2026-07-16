/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    ParserTestCases.h

Abstract:

    X-macro definitions for WSLC CLI parser test cases.

--*/

#pragma once

#include "Argument.h"
#include "ArgumentTypes.h"
#include <string>
#include <vector>

// ArgumentSet enum - defines which set of arguments to use for parsing
enum class ArgumentSet
{
    Run,
    List,
    // RootCommand globals; parsed in optionsOnly mode (stops at first positional).
    Globals,
};

// ParserTestCase - represents a single test case
struct ParserTestCase
{
    ArgumentSet argumentSet;
    bool expectedResult;
    std::wstring commandLine;
};

// True for argument sets that mirror the root-level "global options" parsing
// pass in Main.cpp, which uses optionsOnly=true so the parser stops at the
// first positional / subcommand token without consuming it.
inline bool IsOptionsOnlySet(ArgumentSet argumentSet)
{
    return argumentSet == ArgumentSet::Globals;
}

// Function to get the argument definitions for a given ArgumentSet
inline std::vector<wsl::windows::wslc::Argument> GetArgumentsForSet(ArgumentSet argumentSet)
{
    using namespace wsl::windows::wslc;
    using namespace wsl::windows::wslc::argument;

    switch (argumentSet)
    {
    case ArgumentSet::Run:
        return {
            Argument::Create(ArgType::ImageId, true),  // Required positional argument
            Argument::Create(ArgType::Command, false), // Optional positional argument
            Argument::Create(ArgType::ForwardArgs, false),
            Argument::Create(ArgType::Help),
            Argument::Create(ArgType::Interactive),
            Argument::Create(ArgType::Verbose),
            Argument::Create(ArgType::Remove),
            Argument::Create(ArgType::Signal),
            Argument::Create(ArgType::Time),
            Argument::Create(ArgType::Publish, false, NO_LIMIT), // Not required, unlimited.
        };

    case ArgumentSet::List:
        return {
            Argument::Create(ArgType::ContainerId, false, NO_LIMIT), // Optional positional
            Argument::Create(ArgType::Help),
            Argument::Create(ArgType::Verbose),
        };

    case ArgumentSet::Globals:
        // Synthetic stand-in for what Main.cpp passes as cliGlobals to the
        // first (optionsOnly) parse pass. Decoupled from RootCommand so the
        // parser tests stay stable as the production global set evolves.
        // Quiet (Flag with alias) and Session (Value, no alias) are convenient
        // existing ArgTypes that together exercise both kinds in the global
        // parsing path; the cases below treat them as "the global options".
        return {
            Argument::Create(ArgType::Quiet),
            Argument::Create(ArgType::Session),
        };

    default:
        return {};
    }
}
// X-macro format: WSLC_PARSER_TEST_CASE(ArgumentSetValue, ExpectedResult, CommandLine)
// ArgumentSetValue: Just the enum value name (e.g., Run), without ArgumentSet:: prefix
// ExpectedResult: true if test should succeed, false if it should fail
// CommandLine: The command line string to test

// clang-format off
#define WSLC_PARSER_TEST_CASES \
/* Simple case with required arg and simple other args */ \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -?)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc --verbose image1)") \
\
/* Value tests, flag and non-flag, multi-value */ \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc --publish=80:80 image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc --publish 80:80 image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -p=80:80 image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -p 80:80 image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -p 80:80 -p 443:443 image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -p=80:80 -p=443:443 image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc --verbose --verbose image1)") \
\
/* Flag parse tests */ \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -? image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -?i image1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -i?p- image1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -pi? image1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -pi?=80:80 image1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -pi? 80:80 image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -i?p 80:80 image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -i?p=80:80 image1)") \
\
/* Validation tests */ \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc --signal FOO image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc --signal 9 image1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -t blah image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -t 5 image1)") \
\
/* Multi-positional tests */ \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc image1 command)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc image1 command --f -z forward hello world)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc image1 command forward hello world)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc image1 command forward"hello world")") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc image1 command f="hello world" forward echo)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc --verbose image1 command f="hello world" forward echo)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc image1 \\command\\?"" --f -z forward hello world)") \
\
/* Once the image name is parsed, the next token becomes the optional <command> positional \
 * and everything after that goes into ForwardArgs. Neither <command> nor ForwardArgs are  \
 * interpreted as wslc options. The second case uses '\' + newline between tokens, which   \
 * CommandLineToArgvW passes through as literal '\' tokens that the container shell        \
 * handles correctly. */ \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc jrottenberg/ffmpeg:4.4-alpine ffmpeg -i http://url/to/media.mp4 -stats)") \
WSLC_PARSER_TEST_CASE(Run, true, L"wslc jrottenberg/ffmpeg:4.4-alpine \\\nffmpeg \\\n-i http://url/to/media.mp4 \\\n-stats") \
\
/* Stdin dash ('-') as a positional value. A lone '-' conventionally means stdin and must  \
 * be accepted as a valid positional rather than treated as an invalid flag specifier. */  \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc - command)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc --verbose -)") \
\
/* List cases with multiple args and flags that can come after the optional multi-positional. */ \
WSLC_PARSER_TEST_CASE(List, true, LR"(wslc)") \
WSLC_PARSER_TEST_CASE(List, true, LR"(wslc cont1)") \
WSLC_PARSER_TEST_CASE(List, true, LR"(wslc cont1 cont2)") \
WSLC_PARSER_TEST_CASE(List, true, LR"(wslc --verbose cont1)") \
WSLC_PARSER_TEST_CASE(List, true, LR"(wslc --verbose cont1 cont2)") \
WSLC_PARSER_TEST_CASE(List, true, LR"(wslc cont1 --verbose cont2)") \
WSLC_PARSER_TEST_CASE(List, true, LR"(wslc cont1 cont2 --verbose)") \
\
/* Failure List cases */ \
WSLC_PARSER_TEST_CASE(List, false, LR"(wslc --invalidarg)") \
WSLC_PARSER_TEST_CASE(List, false, LR"(wslc --invalidarg cont1)") \
WSLC_PARSER_TEST_CASE(List, false, LR"(wslc -i cont1 cont2)") \
WSLC_PARSER_TEST_CASE(List, false, LR"(wslc -vp cont1)") \
WSLC_PARSER_TEST_CASE(List, false, LR"(wslc cont1 -v cont2 -12)") \
WSLC_PARSER_TEST_CASE(List, true, LR"(wslc cont1 --verbose=false cont2)") \
WSLC_PARSER_TEST_CASE(List, false, LR"(wslc cont1 --verbose=invalid cont2)") \
WSLC_PARSER_TEST_CASE(List, false, LR"(wslc cont1 cont2 --invalidarg)") \
\
/* Root-level globals: strict optionsOnly parsing. Stops cleanly at the first \
 * non-option token; recognized globals before that are consumed. Production \
 * uses an additional stopOnUnknown bool that is covered separately by       \
 * OptionsOnly_StopOnUnknown_LeavesTokenForCaller. The Globals set is a      \
 * synthetic mix of Quiet (Flag) and Session (Value) — see GetArgumentsForSet \
 * — so these cases test the parser, not RootCommand's current global set.   */ \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc)") \
/* Flag-kind global: long, short, and stops at positional/subcommand.        */ \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc --quiet)") \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc -q)") \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc --quiet image1)") \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc -q image1)") \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc image1)") \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc --quiet system list)") \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc system --verbose)") \
/* Value-kind global: separated and adjoined value forms, then positional.   */ \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc --session foo)") \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc --session=foo)") \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc --session foo image1)") \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc --session=foo image1)") \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc --session foo system list)") \
/* Mixed Flag + Value globals before the first positional.                   */ \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc --quiet --session foo image1)") \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc --session foo -q image1)") \
/* Docker-style idempotency: duplicate global flags collapse to a single entry. */ \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc --quiet --quiet)") \
WSLC_PARSER_TEST_CASE(Globals, true,  LR"(wslc -q -q system list)")
// clang-format on
