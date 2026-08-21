// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "string.hpp"

using wsl::windows::common::string::Ellipsis;
using wsl::windows::common::string::FormatBytes;
using wsl::windows::common::string::FormatHumanReadableSize;
using wsl::windows::common::string::FormatStorageSize;
using wsl::windows::common::string::ParseStorageSize;
using wsl::windows::common::string::StorageSizeUnit;

namespace {

struct StorageSizeFormatCase
{
    uint64_t Bytes;
    StorageSizeUnit Unit;
    uint32_t DecimalPlaces;
    bool IncludeSpace;
    std::wstring Expected;
};

struct StorageSizeTextRoundTripCase
{
    std::wstring Text;
    StorageSizeUnit Unit;
    uint32_t DecimalPlaces;
    bool IncludeSpace;
};

void VerifyDockerStorageSize(const std::string& Input, StorageSizeUnit Unit, std::optional<uint64_t> Expected)
{
    const auto wideInput = wsl::shared::string::MultiByteToWide(Input);
    VERIFY_ARE_EQUAL(Expected, ParseStorageSize(wideInput, Unit));
}

std::vector<std::string> DockerSuffixes(char Unit)
{
    const auto UpperUnit = static_cast<char>(std::toupper(static_cast<unsigned char>(Unit)));
    return {
        {Unit},
        {UpperUnit},
        {Unit, 'b'},
        {Unit, 'B'},
        {UpperUnit, 'b'},
        {UpperUnit, 'B'},
        {Unit, 'i', 'b'},
        {Unit, 'i', 'B'},
        {Unit, 'I', 'b'},
        {Unit, 'I', 'B'},
        {UpperUnit, 'i', 'b'},
        {UpperUnit, 'i', 'B'},
        {UpperUnit, 'I', 'b'},
        {UpperUnit, 'I', 'B'},
    };
}

void VerifyDockerStorageUnits(StorageSizeUnit Unit, uint64_t Base)
{
    uint64_t Factor = Base;
    for (const auto UnitName : {'k', 'm', 'g', 't', 'p'})
    {
        for (const auto& Suffix : DockerSuffixes(UnitName))
        {
            VerifyDockerStorageSize("32" + Suffix, Unit, 32 * Factor);
        }

        Factor *= Base;
    }
}

} // namespace

namespace StringUnitTests {
class StringUnitTests
{
    WSL_TEST_CLASS(StringUnitTests)

    TEST_METHOD(ParseMemorySize_LegacyForms)
    {
        const std::vector<std::pair<LPCSTR, std::optional<uint64_t>>> TestCases{
            {"0", 0},
            {"1", 1},
            {" 1", 1},
            {"1B", 1},
            {"1K", 1024},
            {"1KB", 1024},
            {"2M", 2 * 1024 * 1024},
            {"100MB", 100 * 1024 * 1024},
            {"9G", 9 * 1024ULL * 1024ULL * 1024ULL},
            {"44GB", 44 * 1024ULL * 1024ULL * 1024ULL},
            {"1TB", 1ULL << 40},
            {"2T", 2ULL << 40},
            {"1 B", std::nullopt},
            {nullptr, std::nullopt},
            {"", std::nullopt},
            {"foo", std::nullopt}};

        for (const auto& [Input, Expected] : TestCases)
        {
            VERIFY_ARE_EQUAL(Expected, wsl::shared::string::ParseMemorySize(Input));

            const auto wideInput = wsl::shared::string::MultiByteToWide(Input);
            VERIFY_ARE_EQUAL(Expected, wsl::shared::string::ParseMemorySize(wideInput.c_str()));
        }
    }

    TEST_METHOD(ParseStorageSize_DockerDecimalUnits)
    {
        VerifyDockerStorageUnits(StorageSizeUnit::Decimal, 1000);
    }

    TEST_METHOD(ParseStorageSize_DockerBinaryUnits)
    {
        VerifyDockerStorageUnits(StorageSizeUnit::Binary, 1024);
    }

    TEST_METHOD(ParseStorageSize_DockerNumericForms)
    {
        for (const auto Unit : {StorageSizeUnit::Decimal, StorageSizeUnit::Binary})
        {
            VerifyDockerStorageSize("0", Unit, 0);
            VerifyDockerStorageSize("0b", Unit, 0);
            VerifyDockerStorageSize("0B", Unit, 0);
            VerifyDockerStorageSize("0 B", Unit, 0);
            VerifyDockerStorageSize("32", Unit, 32);
            VerifyDockerStorageSize("32b", Unit, 32);
            VerifyDockerStorageSize("32B", Unit, 32);
            VerifyDockerStorageSize("32.5 B", Unit, 32);
            VerifyDockerStorageSize("0.", Unit, 0);
            VerifyDockerStorageSize("0. ", Unit, 0);
            VerifyDockerStorageSize("0.b", Unit, 0);
            VerifyDockerStorageSize("0.B", Unit, 0);
            VerifyDockerStorageSize("-0", Unit, 0);
            VerifyDockerStorageSize("-0b", Unit, 0);
            VerifyDockerStorageSize("-0B", Unit, 0);
            VerifyDockerStorageSize("-0 b", Unit, 0);
            VerifyDockerStorageSize("-0 B", Unit, 0);
            VerifyDockerStorageSize("+32K", Unit, 32 * (Unit == StorageSizeUnit::Decimal ? 1000 : 1024));
            VerifyDockerStorageSize("1e3K", Unit, 1000 * (Unit == StorageSizeUnit::Decimal ? 1000 : 1024));
            VerifyDockerStorageSize("32.", Unit, 32);
            VerifyDockerStorageSize("32.b", Unit, 32);
            VerifyDockerStorageSize("32.B", Unit, 32);
            VerifyDockerStorageSize("32. b", Unit, 32);
            VerifyDockerStorageSize("32. B", Unit, 32);
            VerifyDockerStorageSize("9007199254740991", Unit, 9'007'199'254'740'991);
            VerifyDockerStorageSize("9007199254740992", Unit, 9'007'199'254'740'992);
            VerifyDockerStorageSize("9007199254740993", Unit, 9'007'199'254'740'993);
            VerifyDockerStorageSize("9223372036854775806", Unit, 9'223'372'036'854'775'806);
            VerifyDockerStorageSize("9223372036854775807", Unit, 9'223'372'036'854'775'807);
            VerifyDockerStorageSize("9223372036854775808", Unit, 9'223'372'036'854'775'808ULL);
            VerifyDockerStorageSize("18446744073709551615", Unit, std::numeric_limits<uint64_t>::max());
        }

        VerifyDockerStorageSize("32.5kB", StorageSizeUnit::Decimal, 32'500);
        VerifyDockerStorageSize("32.5 kB", StorageSizeUnit::Decimal, 32'500);
        VerifyDockerStorageSize("0.3 K", StorageSizeUnit::Decimal, 300);
        VerifyDockerStorageSize(".3kB", StorageSizeUnit::Decimal, 300);
        VerifyDockerStorageSize("32.3 mb", StorageSizeUnit::Binary, 33'869'004);
        VerifyDockerStorageSize("0.3MB", StorageSizeUnit::Binary, 314'572);
        VerifyDockerStorageSize("18446744073709551K", StorageSizeUnit::Decimal, 18'446'744'073'709'551'000ULL);
        VerifyDockerStorageSize("18446744073709552K", StorageSizeUnit::Decimal, std::nullopt);
        VerifyDockerStorageSize("18014398509481983K", StorageSizeUnit::Binary, 18'446'744'073'709'550'592ULL);
        VerifyDockerStorageSize("18014398509481984K", StorageSizeUnit::Binary, std::nullopt);
    }

    TEST_METHOD(ParseStorageSize_DockerInvalidForms)
    {
        const std::vector<std::string> InvalidSizes{
            "",       "hello",
            ".",      ". ",
            " ",      "  ",
            " .",     " . ",
            " 0",     " 0b",
            " 0B",    " 0 B",
            "0b ",    "0B ",
            "0 B ",   "-32",
            "-32b",   "-32B",
            "-32 b",  "-32 B",
            "32b.",   "32B.",
            "32 b.",  "32 B.",
            "32 bb",  "32 BB",
            "32 b b", "32 B B",
            "32  b",  "32  B",
            " 32 ",   "32m b",
            "32bm",   "1E",
            "1EB",    "1EiB",
            "1e309",  "18446744073709551616",
        };

        for (const auto Unit : {StorageSizeUnit::Decimal, StorageSizeUnit::Binary})
        {
            for (const auto& Input : InvalidSizes)
            {
                VerifyDockerStorageSize(Input, Unit, std::nullopt);
            }
        }
    }

    TEST_METHOD(FormatStorageSize_UsesRequestedPrecision)
    {
        const std::vector<StorageSizeFormatCase> TestCases{
            {0, StorageSizeUnit::Decimal, 0, false, L"0B"},
            {999, StorageSizeUnit::Decimal, 2, true, L"999 B"},
            {1'000, StorageSizeUnit::Decimal, 0, false, L"1KB"},
            {119'856'765, StorageSizeUnit::Decimal, 0, false, L"120MB"},
            {119'856'765, StorageSizeUnit::Decimal, 1, false, L"119.9MB"},
            {119'856'765, StorageSizeUnit::Decimal, 2, false, L"119.86MB"},
            {1'000'000'000'000ULL, StorageSizeUnit::Decimal, 2, false, L"1.00TB"},
            {1'000'000'000'000'000ULL, StorageSizeUnit::Decimal, 2, false, L"1.00PB"},
            {1'000'000'000'000'000'000ULL, StorageSizeUnit::Decimal, 2, false, L"1000.00PB"},
            {1'023, StorageSizeUnit::Binary, 2, true, L"1023 B"},
            {1'024, StorageSizeUnit::Binary, 0, false, L"1KiB"},
            {1'536, StorageSizeUnit::Binary, 1, false, L"1.5KiB"},
            {1'610'612'736, StorageSizeUnit::Binary, 0, false, L"2GiB"},
            {1'610'612'736, StorageSizeUnit::Binary, 1, false, L"1.5GiB"},
            {1ULL << 40, StorageSizeUnit::Binary, 2, false, L"1.00TiB"},
            {1ULL << 50, StorageSizeUnit::Binary, 2, false, L"1.00PiB"},
            {1ULL << 60, StorageSizeUnit::Binary, 2, false, L"1024.00PiB"},
        };

        for (const auto& TestCase : TestCases)
        {
            VERIFY_ARE_EQUAL(TestCase.Expected, FormatStorageSize(TestCase.Bytes, TestCase.Unit, TestCase.DecimalPlaces, TestCase.IncludeSpace));
        }

        VERIFY_ARE_EQUAL(std::wstring{L"119.86 MB"}, FormatBytes(119'856'765));
    }

    // Image sizes are rendered with three significant digits, base 1000, no space, and "kB" rather
    // than "KB".
    TEST_METHOD(FormatHumanReadableSize_MatchesImageSizePrecision)
    {
        const std::vector<std::pair<uint64_t, std::wstring>> TestCases{
            {0, L"0B"},
            {999, L"999B"},
            {1'000, L"1kB"},
            {1'500, L"1.5kB"},
            {7'050'000, L"7.05MB"},
            {119'856'765, L"120MB"},
            {1'090'000'000, L"1.09GB"},
            {1'000'000'000'000ULL, L"1TB"},
        };

        for (const auto& [bytes, expected] : TestCases)
        {
            VERIFY_ARE_EQUAL(expected, FormatHumanReadableSize(bytes));
        }

        // Three significant digits switch to exponent form just below the next unit, matching Go's %g.
        VERIFY_ARE_EQUAL(std::wstring{L"1e+03MB"}, FormatHumanReadableSize(999'900'000));
    }

    TEST_METHOD(FormatHumanReadableSize_SupportsReclaimedSpacePrecision)
    {
        const std::vector<std::pair<uint64_t, std::wstring>> TestCases{
            {0, L"0B"},
            {999, L"999B"},
            {12'288, L"12.29kB"},
            {119'856'765, L"119.9MB"},
            {1'090'000'000, L"1.09GB"},
        };

        for (const auto& [bytes, expected] : TestCases)
        {
            VERIFY_ARE_EQUAL(expected, FormatHumanReadableSize(bytes, 4));
        }
    }

    // Docker shortens display values with formatter.Ellipsis, which measures terminal columns rather than
    // characters so East Asian wide and fullwidth code points count double.
    TEST_METHOD(Ellipsis_NarrowCharacters_AreCountedAsOneColumn)
    {
        VERIFY_ARE_EQUAL(std::wstring{L""}, Ellipsis(L"", 20));
        VERIFY_ARE_EQUAL(std::wstring{L"sleep 3600"}, Ellipsis(L"sleep 3600", 20));
        VERIFY_ARE_EQUAL(std::wstring{L"12345678901234567890"}, Ellipsis(L"12345678901234567890", 20));
        VERIFY_ARE_EQUAL(std::wstring{L"1234567890123456789\u2026"}, Ellipsis(L"123456789012345678901", 20));
        VERIFY_ARE_EQUAL(std::wstring(19, L'\u00E0') + L"\u2026", Ellipsis(std::wstring(21, L'\u00E0'), 20));
    }

    TEST_METHOD(Ellipsis_WideCharacters_AreCountedAsTwoColumns)
    {
        // Ten wide characters fill the twenty columns exactly, so an eleventh forces the value to be shortened
        // to the nine characters that leave room for the ellipsis.
        VERIFY_ARE_EQUAL(std::wstring(10, L'\u65E5'), Ellipsis(std::wstring(10, L'\u65E5'), 20));
        VERIFY_ARE_EQUAL(std::wstring(9, L'\u65E5') + L"\u2026", Ellipsis(std::wstring(11, L'\u65E5'), 20));
        VERIFY_ARE_EQUAL(std::wstring(9, L'\uFF21') + L"\u2026", Ellipsis(std::wstring(11, L'\uFF21'), 20));
        VERIFY_ARE_EQUAL(std::wstring{L"ab"} + std::wstring(8, L'\u65E5') + L"\u2026", Ellipsis(L"ab" + std::wstring(10, L'\u65E5'), 20));
    }

    TEST_METHOD(Ellipsis_SurrogatePairs_AreNotSplit)
    {
        // Emoji are wide and encoded as surrogate pairs, so both the column count and the code unit boundary
        // have to be honored.
        const std::wstring emoji{L"\U0001F600"};
        std::wstring ten;
        for (size_t index = 0; index < 10; ++index)
        {
            ten += emoji;
        }

        VERIFY_ARE_EQUAL(ten, Ellipsis(ten, 20));
        VERIFY_ARE_EQUAL(ten.substr(0, 18) + L"\u2026", Ellipsis(ten + emoji, 20));
    }

    TEST_METHOD(Ellipsis_SmallWidths_MatchDocker)
    {
        VERIFY_ARE_EQUAL(std::wstring{L""}, Ellipsis(L"abc", 0));

        // A width of one has no room for both content and an ellipsis, so the leading code point is kept even
        // when it is wider than the limit.
        VERIFY_ARE_EQUAL(std::wstring{L"a"}, Ellipsis(L"abc", 1));
        VERIFY_ARE_EQUAL(std::wstring{L"\u65E5"}, Ellipsis(L"\u65E5\u65E5", 1));
        VERIFY_ARE_EQUAL(std::wstring{L"\U0001F600"}, Ellipsis(L"\U0001F600\U0001F600", 1));

        // A leading wide character leaves no room for the ellipsis at a width of two, and docker returns the
        // value untouched in that case.
        VERIFY_ARE_EQUAL(std::wstring{L"\u65E5\u65E5"}, Ellipsis(L"\u65E5\u65E5", 2));
        VERIFY_ARE_EQUAL(std::wstring{L"a\u2026"}, Ellipsis(L"abc", 2));
        VERIFY_ARE_EQUAL(std::wstring{L"\u65E5\u2026"}, Ellipsis(L"\u65E5\u65E5", 3));
    }

    TEST_METHOD(StorageSize_BytesToTextRoundTrips)
    {
        const auto VerifyRoundTrip = [](uint64_t Bytes, StorageSizeUnit Unit, uint32_t DecimalPlaces, bool IncludeSpace = false) {
            const auto text = FormatStorageSize(Bytes, Unit, DecimalPlaces, IncludeSpace);
            VERIFY_ARE_EQUAL(std::optional<uint64_t>{Bytes}, ParseStorageSize(text, Unit));
        };

        VerifyRoundTrip(0, StorageSizeUnit::Decimal, 0);
        VerifyRoundTrip(32, StorageSizeUnit::Decimal, 0, true);
        VerifyRoundTrip(1'500, StorageSizeUnit::Decimal, 1);
        VerifyRoundTrip(1'536, StorageSizeUnit::Binary, 1);
        VerifyRoundTrip(1'000'000'000'000'000'000ULL, StorageSizeUnit::Decimal, 2);
        VerifyRoundTrip(1ULL << 60, StorageSizeUnit::Binary, 2);

        uint64_t decimalFactor = 1'000;
        uint64_t binaryFactor = 1'024;
        for (size_t index = 0; index < 5; ++index)
        {
            VerifyRoundTrip(32 * decimalFactor, StorageSizeUnit::Decimal, 0);
            VerifyRoundTrip(32 * binaryFactor, StorageSizeUnit::Binary, 0, true);
            decimalFactor *= 1'000;
            binaryFactor *= 1'024;
        }
    }

    TEST_METHOD(StorageSize_TextToBytesRoundTrips)
    {
        const std::vector<StorageSizeTextRoundTripCase> TestCases{
            {L"0B", StorageSizeUnit::Decimal, 0, false},
            {L"32 B", StorageSizeUnit::Decimal, 0, true},
            {L"32KB", StorageSizeUnit::Decimal, 0, false},
            {L"32.5MB", StorageSizeUnit::Decimal, 1, false},
            {L"1GB", StorageSizeUnit::Decimal, 0, false},
            {L"1.25TB", StorageSizeUnit::Decimal, 2, false},
            {L"1PB", StorageSizeUnit::Decimal, 0, false},
            {L"32KiB", StorageSizeUnit::Binary, 0, false},
            {L"1.5MiB", StorageSizeUnit::Binary, 1, false},
            {L"1GiB", StorageSizeUnit::Binary, 0, false},
            {L"1.25 TiB", StorageSizeUnit::Binary, 2, true},
            {L"1PiB", StorageSizeUnit::Binary, 0, false},
        };

        for (const auto& TestCase : TestCases)
        {
            const auto bytes = ParseStorageSize(TestCase.Text, TestCase.Unit);
            VERIFY_IS_TRUE(bytes.has_value());

            const auto text = FormatStorageSize(bytes.value(), TestCase.Unit, TestCase.DecimalPlaces, TestCase.IncludeSpace);
            VERIFY_ARE_EQUAL(TestCase.Text, text);
            VERIFY_ARE_EQUAL(bytes, ParseStorageSize(text, TestCase.Unit));
        }
    }
};
} // namespace StringUnitTests
