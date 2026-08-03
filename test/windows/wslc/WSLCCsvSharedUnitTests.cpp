// Copyright (C) Microsoft Corporation. All rights reserved.

/*++

Module Name:

    WSLCCsvSharedUnitTests.cpp

Abstract:

    Unit tests for the shared CSV helpers in wsl::shared::string (SplitCsvFields, CsvEscapeField,
    JoinCsvFields). These implement the single-record RFC 4180 grammar that docker buildx uses via
    go-csvvalue / encoding/csv, and back the --output/--secret spec parsers. The tests pin the grammar
    (quoting, "" escaping, embedded commas, malformed-record rejection) and the escape/join round-trip.

--*/

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"
#include <optional>
#include <string>
#include <vector>

namespace WSLCCsvSharedUnitTests {

using WStrings = std::vector<std::wstring>;

class WSLCCsvSharedUnitTests
{
    WSLC_TEST_CLASS(WSLCCsvSharedUnitTests)

    static void VerifySplit(const std::wstring& record, const WStrings& expected)
    {
        const auto fields = wsl::shared::string::SplitCsvFields(record);
        VERIFY_IS_TRUE(fields.has_value());
        if (!fields.has_value())
        {
            return;
        }

        VERIFY_ARE_EQUAL(expected.size(), fields->size());
        for (size_t i = 0; i < expected.size() && i < fields->size(); ++i)
        {
            VERIFY_ARE_EQUAL(expected[i], (*fields)[i]);
        }
    }

    static void VerifyMalformed(const std::wstring& record)
    {
        VERIFY_IS_FALSE(wsl::shared::string::SplitCsvFields(record).has_value());
    }

    // --- SplitCsvFields ---

    TEST_METHOD(Split_SimpleFields)
    {
        VerifySplit(L"a,b,c", WStrings{L"a", L"b", L"c"});
    }

    TEST_METHOD(Split_SingleField)
    {
        VerifySplit(L"abc", WStrings{L"abc"});
    }

    TEST_METHOD(Split_EmptyInputIsOneEmptyField)
    {
        VerifySplit(L"", WStrings{L""});
    }

    TEST_METHOD(Split_EmptyFieldsPreserved)
    {
        // Unlike wsl::shared::string::Split, empty fields are preserved (they are meaningful in a spec).
        VerifySplit(L"a,,c", WStrings{L"a", L"", L"c"});
    }

    TEST_METHOD(Split_LeadingAndTrailingCommas)
    {
        VerifySplit(L",a,", WStrings{L"", L"a", L""});
    }

    TEST_METHOD(Split_QuotedFieldWithComma)
    {
        VerifySplit(L"\"a,b\",c", WStrings{L"a,b", L"c"});
    }

    TEST_METHOD(Split_QuotedFieldWithEscapedQuote)
    {
        // A doubled quote inside a quoted field is a single literal quote.
        VerifySplit(L"\"a\"\"b\"", WStrings{L"a\"b"});
    }

    TEST_METHOD(Split_QuotedEmptyField)
    {
        VerifySplit(L"\"\",x", WStrings{L"", L"x"});
    }

    TEST_METHOD(Split_QuotedThenPlainField)
    {
        VerifySplit(L"\"a\",b", WStrings{L"a", L"b"});
    }

    TEST_METHOD(Split_UnterminatedQuoteIsMalformed)
    {
        VerifyMalformed(L"\"abc");
    }

    TEST_METHOD(Split_TextAfterClosingQuoteIsMalformed)
    {
        VerifyMalformed(L"\"a\"b");
    }

    TEST_METHOD(Split_BareQuoteInUnquotedFieldIsMalformed)
    {
        // Go's encoding/csv (non-lazy) rejects a bare double quote in an unquoted field (ErrBareQuote).
        VerifyMalformed(L"a\"b,c");
    }

    // --- CsvEscapeField ---

    TEST_METHOD(Escape_PlainFieldUnchanged)
    {
        VERIFY_ARE_EQUAL(std::wstring(L"abc"), wsl::shared::string::CsvEscapeField(std::wstring(L"abc")));
    }

    TEST_METHOD(Escape_EmptyFieldUnchanged)
    {
        VERIFY_ARE_EQUAL(std::wstring(L""), wsl::shared::string::CsvEscapeField(std::wstring(L"")));
    }

    TEST_METHOD(Escape_CommaQuoted)
    {
        VERIFY_ARE_EQUAL(std::wstring(L"\"a,b\""), wsl::shared::string::CsvEscapeField(std::wstring(L"a,b")));
    }

    TEST_METHOD(Escape_QuoteDoubledAndQuoted)
    {
        VERIFY_ARE_EQUAL(std::wstring(L"\"a\"\"b\""), wsl::shared::string::CsvEscapeField(std::wstring(L"a\"b")));
    }

    TEST_METHOD(Escape_NewlineQuoted)
    {
        VERIFY_ARE_EQUAL(std::wstring(L"\"a\nb\""), wsl::shared::string::CsvEscapeField(std::wstring(L"a\nb")));
    }

    TEST_METHOD(Escape_LeadingOrTrailingSpaceQuoted)
    {
        VERIFY_ARE_EQUAL(std::wstring(L"\" a\""), wsl::shared::string::CsvEscapeField(std::wstring(L" a")));
        VERIFY_ARE_EQUAL(std::wstring(L"\"a \""), wsl::shared::string::CsvEscapeField(std::wstring(L"a ")));
    }

    // --- JoinCsvFields ---

    TEST_METHOD(Join_PlainFields)
    {
        VERIFY_ARE_EQUAL(std::wstring(L"a,b,c"), wsl::shared::string::JoinCsvFields(WStrings{L"a", L"b", L"c"}));
    }

    TEST_METHOD(Join_EscapesFieldContainingComma)
    {
        VERIFY_ARE_EQUAL(std::wstring(L"\"a,b\",c"), wsl::shared::string::JoinCsvFields(WStrings{L"a,b", L"c"}));
    }

    // --- Round-trip: SplitCsvFields(JoinCsvFields(x)) == x ---

    TEST_METHOD(RoundTrip_JoinThenSplitPreservesFields)
    {
        const WStrings inputs{L"type=image", L"annotation.foo=a,b,c", L"name=x", L"quote=a\"b", L""};
        const auto joined = wsl::shared::string::JoinCsvFields(inputs);
        VerifySplit(joined, inputs);
    }

    // --- Template works for narrow (char) strings too ---

    TEST_METHOD(Narrow_SplitAndJoinRoundTrip)
    {
        const std::vector<std::string> inputs{"a,b", "c", "d\"e"};
        const auto joined = wsl::shared::string::JoinCsvFields(inputs);
        VERIFY_ARE_EQUAL(std::string("\"a,b\",c,\"d\"\"e\""), joined);

        const auto fields = wsl::shared::string::SplitCsvFields(joined);
        VERIFY_IS_TRUE(fields.has_value());
        VERIFY_ARE_EQUAL(inputs.size(), fields->size());
        for (size_t i = 0; i < inputs.size() && i < fields->size(); ++i)
        {
            VERIFY_ARE_EQUAL(inputs[i], (*fields)[i]);
        }
    }
};

} // namespace WSLCCsvSharedUnitTests
