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
#include "VolumeModel.h"

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
            {L"=value", "", "value"},
            {L"", "", ""},
            {L"key=a=b=c", "key", "a=b=c"},
        };

        for (const auto& [input, expectedKey, expectedValue] : validOptions)
        {
            auto result = models::DriverOption::Parse(input);
            VERIFY_ARE_EQUAL(expectedKey, result.Key());
            VERIFY_ARE_EQUAL(expectedValue, result.Value());
        }
    }
};

} // namespace WSLCCLIOptionsParserUnitTests
