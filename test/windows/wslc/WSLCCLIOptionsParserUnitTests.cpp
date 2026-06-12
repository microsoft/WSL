/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLIOptionsParserUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI driver option parsing.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "ArgumentValidation.h"

using namespace wsl::windows::wslc;

namespace WSLCCLIOptionsParserUnitTests {

class WSLCCLIOptionsParserUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIOptionsParserUnitTests)

    TEST_METHOD(WSLCCLIOptionsParser_ValidOptions)
    {
        std::vector<std::tuple<std::wstring, std::string, std::string>> validOptions = {
            {L"SizeBytes=3145728", "SizeBytes", "3145728"},
            {L"ReadOnly", "ReadOnly", ""},
            {L"key=", "key", ""},
            {L"key=a=b=c", "key", "a=b=c"},
        };

        for (const auto& [input, expectedKey, expectedValue] : validOptions)
        {
            auto result = validation::ParseDriverOption(input);
            VERIFY_ARE_EQUAL(expectedKey, result.first);
            VERIFY_ARE_EQUAL(expectedValue, result.second);
        }
    }

    TEST_METHOD(WSLCCLIOptionsParser_InvalidOptions)
    {
        std::vector<std::wstring> invalidOptions = {
            L"",
            L"=",
            L"=value",
        };

        for (const auto& input : invalidOptions)
        {
            try
            {
                (void)validation::ParseDriverOption(input);
                VERIFY_FAIL(L"Expected exception");
            }
            catch (const wil::ResultException& ex)
            {
                VERIFY_ARE_EQUAL(E_INVALIDARG, ex.GetErrorCode());

                const auto raw = ex.GetFailureInfo().pszMessage;
                std::wstring message = raw ? raw : L"";
                VERIFY_ARE_EQUAL(L"Driver option key cannot be empty", message);
            }
        }
    }
};

} // namespace WSLCCLIOptionsParserUnitTests
