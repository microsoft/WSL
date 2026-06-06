/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIResourceLimitsParserUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI resource-limit (--cpus, --memory, --ulimit) parsing and validation.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "ArgumentValidation.h"

using namespace wsl::windows::wslc;

namespace WSLCCLIResourceLimitsParserUnitTests {

class WSLCCLIResourceLimitsParserUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIResourceLimitsParserUnitTests)

    TEST_METHOD(NanoCpus_Valid)
    {
        // (input, expected nanoCpus)
        std::vector<std::pair<std::wstring, int64_t>> valid = {
            {L"1", 1'000'000'000LL},
            {L"2", 2'000'000'000LL},
            {L"0.5", 500'000'000LL},
            {L"1.5", 1'500'000'000LL},
            {L"2.5", 2'500'000'000LL},
            {L"0.001", 1'000'000LL},
        };

        for (const auto& [input, expected] : valid)
        {
            const auto actual = validation::GetNanoCpusFromString(input, L"cpus");
            VERIFY_ARE_EQUAL(expected, actual);
        }
    }

    TEST_METHOD(NanoCpus_Invalid)
    {
        // Each value should be rejected as an invalid --cpus value.
        const std::vector<std::wstring> invalid = {
            L"",
            L"0",          // not positive
            L"-1",         // sign char rejected
            L"-0.5",       // sign char rejected
            L"abc",        // not numeric
            L"1.5x",       // trailing garbage
            L" 1",         // leading whitespace
            L"1 ",         // trailing whitespace
            L"1e3",        // exponent not allowed
            L"+1",         // sign char rejected
            L"1.2.3",      // multiple dots (rejected by strtod's strict end check)
            L"99999999999" // overflow when multiplied by 1e9
        };

        for (const auto& input : invalid)
        {
            VERIFY_THROWS(validation::GetNanoCpusFromString(input, L"cpus"), ArgumentException);
        }
    }

    TEST_METHOD(Ulimit_Valid)
    {
        // (input, expectedName, expectedSoft, expectedHard)
        std::vector<std::tuple<std::wstring, std::string, int64_t, int64_t>> valid = {
            {L"nofile=1024", "nofile", 1024, 1024},
            {L"nofile=1024:2048", "nofile", 1024, 2048},
            {L"nproc=512:512", "nproc", 512, 512},
            {L"core=-1", "core", -1, -1},
            {L"core=-1:-1", "core", -1, -1},
            {L"memlock=0", "memlock", 0, 0},
            {L"stack=8192:-1", "stack", 8192, -1},
        };

        for (const auto& [input, expectedName, expectedSoft, expectedHard] : valid)
        {
            const auto [name, soft, hard] = validation::ParseUlimit(input, L"ulimit");
            VERIFY_ARE_EQUAL(expectedName, name);
            VERIFY_ARE_EQUAL(expectedSoft, soft);
            VERIFY_ARE_EQUAL(expectedHard, hard);
        }
    }

    TEST_METHOD(Ulimit_Invalid)
    {
        const std::vector<std::wstring> invalid = {
            L"",
            L"=1024",                         // empty name
            L"nofile=",                       // empty value
            L"nofile",                        // missing '='
            L"nofile=abc",                    // non-numeric soft
            L"nofile=1024:",                  // empty hard
            L"nofile=:1024",                  // empty soft
            L"nofile=-2",                     // negative other than -1
            L"nofile=1024:512",               // hard < soft (and both positive)
            L"nofile=-1:1024",                // unlimited soft but limited hard
            L"nofile=-1:9223372036854775807", // unlimited soft but finite (INT64_MAX) hard
            L"nofile=1.5",                    // not integer
        };

        for (const auto& input : invalid)
        {
            VERIFY_THROWS(validation::ParseUlimit(input, L"ulimit"), ArgumentException);
        }
    }

    TEST_METHOD(NanoCpus_Validator)
    {
        VERIFY_NO_THROW(validation::ValidateNanoCpus({L"0.5", L"1", L"2.5"}, L"cpus"));
        VERIFY_THROWS(validation::ValidateNanoCpus({L"1", L"0"}, L"cpus"), ArgumentException);
    }

    TEST_METHOD(Ulimit_Validator)
    {
        VERIFY_NO_THROW(validation::ValidateUlimit({L"nofile=1024", L"core=-1"}, L"ulimit"));
        VERIFY_THROWS(validation::ValidateUlimit({L"nofile=1024", L"bad"}, L"ulimit"), ArgumentException);
    }
};

} // namespace WSLCCLIResourceLimitsParserUnitTests
