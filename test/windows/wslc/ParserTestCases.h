/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIParserTestCases.h

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
            Argument::Create(ArgType::ContainerId, true), // Required positional argument
            Argument::Create(ArgType::Command, false),    // Optional positional argument
            Argument::Create(ArgType::ForwardArgs, false),
            Argument::Create(ArgType::Help),
            Argument::Create(ArgType::Interactive),
            Argument::Create(ArgType::Verbose),
            Argument::Create(ArgType::Remove),
            Argument::Create(ArgType::Publish, false, 3), // Not required, up to 3 values.
        };
    
    case ArgumentSet::List:
        return {
            Argument::Create(ArgType::ContainerId, false, 10), // Optional positional
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
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -?)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc cont1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc --verbose cont1)") \
\
/* Value tests, flag and non-flag, multi-value */ \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc --publish=80:80 cont1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc --publish 80:80 cont1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -p=80:80 cont1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -p 80:80 cont1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -p 80:80 -p 443:443 cont1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -p=80:80 -p=443:443 cont1)") \
\
/* Flag parse tests */ \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -v cont1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -vi cont1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -rm cont1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -virm cont1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -vrmi cont1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -rmiv cont1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -rmiv- cont1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -rmivp- cont1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -prmiv cont1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -prmiv=80:80 cont1)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc -prmiv 80:80 cont1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -rmivp 80:80 cont1)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc -rmivp=80:80 cont1)") \
\
/* Multi-positional tests */ \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc cont1 command)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc cont1 command --f -z forward hello world)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc cont1 command forward hello world)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc cont1 command forward"hello world")") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc cont1 command f="hello world" forward echo)") \
WSLC_PARSER_TEST_CASE(Run, false, LR"(wslc cont1 -v command f="hello world" forward echo)") \
WSLC_PARSER_TEST_CASE(Run, true, LR"(wslc cont1 \\command\\?"" --f -z forward hello world)") \
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