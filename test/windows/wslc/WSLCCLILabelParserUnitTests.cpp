/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCCLILabelParserUnitTests.cpp

Abstract:

    This file contains unit tests for WSLC CLI label parsing and validation.
--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include "VolumeModel.h"

using namespace wsl::windows::wslc;

namespace WSLCCLILabelParserUnitTests {

class WSLCCLILabelParserUnitTests
{
    WSLC_TEST_CLASS(WSLCCLILabelParserUnitTests)

    TEST_METHOD(WSLCCLILabelParser_ValidLabels)
    {
        std::vector<std::tuple<std::wstring, std::string, std::string>> validLabels = {
            {L"foo=bar", "foo", "bar"},
            {L"foo=", "foo", ""},
            {L"foo", "foo", ""},
            {L"foo=a=b=c", "foo", "a=b=c"},
        };

        for (const auto& [input, expectedKey, expectedValue] : validLabels)
        {
            auto result = models::Label::Parse(input);
            VERIFY_ARE_EQUAL(expectedKey, result.Key());
            VERIFY_ARE_EQUAL(expectedValue, result.Value());
        }
    }

    TEST_METHOD(WSLCCLILabelParser_InvalidLabels)
    {
        std::vector<std::wstring> invalidLabels = {
            L"",
            L"=",
            L"=value",
        };

        for (const auto& input : invalidLabels)
        {
            try
            {
                (void)models::Label::Parse(input);
                VERIFY_FAIL(L"Expected exception");
            }
            catch (const wil::ResultException& ex)
            {
                VERIFY_ARE_EQUAL(E_INVALIDARG, ex.GetErrorCode());

                const auto raw = ex.GetFailureInfo().pszMessage;
                std::wstring message = raw ? raw : L"";
                VERIFY_ARE_EQUAL(L"Label key cannot be empty", message);
            }
        }
    }
};

} // namespace WSLCCLILabelParserUnitTests
