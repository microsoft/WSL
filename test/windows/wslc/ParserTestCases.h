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
};

// ParserTestCase - represents a single test case
struct ParserTestCase
{
    ArgumentSet argumentSet;
    bool expectedResult;
    std::wstring commandLine;
};

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
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -h)") \
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
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc --verbose --verbose image1)") \
\
/* Flag parse tests */ \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -h image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -hi image1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -ihp- image1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -pih image1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -pih=80:80 image1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -pih 80:80 image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -ihp 80:80 image1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -ihp=80:80 image1)") \
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
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -v image1 command f="hello world" forward echo)") \
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
WSLC_PARSER_TEST_CASE(List, false, LR"(wslc cont1 --verbose=false cont2)") \
WSLC_PARSER_TEST_CASE(List, false, LR"(wslc cont1 cont2 --invalidarg)")
// clang-format on