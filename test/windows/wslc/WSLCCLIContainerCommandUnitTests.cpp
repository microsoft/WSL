// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "ContainerService.h"

using namespace wsl::windows::wslc;
using namespace wsl::windows::wslc::services;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIContainerCommandUnitTests {

class WSLCCLIContainerCommandUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIContainerCommandUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    static std::wstring Truncated(const std::string& command)
    {
        return ContainerService::FormatCommand(command, true);
    }

    TEST_METHOD(FormatCommand_Empty_ReturnsEmptyQuotes)
    {
        VERIFY_ARE_EQUAL(std::wstring{LR"("")"}, Truncated(""));
    }

    TEST_METHOD(FormatCommand_ShortCommand_IsQuotedUnchanged)
    {
        VERIFY_ARE_EQUAL(std::wstring{LR"("sleep 3600")"}, Truncated("sleep 3600"));
    }

    TEST_METHOD(FormatCommand_ExactlyTwentyCharacters_IsNotShortened)
    {
        VERIFY_ARE_EQUAL(std::wstring{LR"("12345678901234567890")"}, Truncated("12345678901234567890"));
    }

    TEST_METHOD(FormatCommand_TwentyOneCharacters_KeepsNineteenAndAppendsEllipsis)
    {
        VERIFY_ARE_EQUAL(std::wstring{L"\"1234567890123456789\u2026\""}, Truncated("123456789012345678901"));
    }

    TEST_METHOD(FormatCommand_LongCommand_MatchesDockerOutput)
    {
        VERIFY_ARE_EQUAL(std::wstring{L"\"sh -c 'echo this is\u2026\""}, Truncated("sh -c 'echo this is a very long command that should be truncated by docker'"));
    }

    TEST_METHOD(FormatCommand_NoTruncate_KeepsFullCommand)
    {
        const std::string command = "sh -c 'echo this is a very long command that should be truncated by docker'";
        VERIFY_ARE_EQUAL(
            std::wstring{L"\"" + wsl::shared::string::MultiByteToWide(command) + L"\""}, ContainerService::FormatCommand(command, false));
    }

    TEST_METHOD(FormatCommand_EmbeddedQuotesAndBackslashes_AreEscaped)
    {
        // Raw string literals are avoided here: the compiler mangles them when the verify macro
        // stringizes its arguments.
        VERIFY_ARE_EQUAL(std::wstring{L"\"say \\\"hi\\\"\""}, Truncated("say \"hi\""));
        VERIFY_ARE_EQUAL(std::wstring{L"\"c:\\\\temp\""}, Truncated("c:\\temp"));
    }

    TEST_METHOD(FormatCommand_NarrowMultiByteCharacters_CountedAsOneColumn)
    {
        const std::string accented = "\xC3\xA0";
        std::string twenty;
        for (int i = 0; i < 20; ++i)
        {
            twenty += accented;
        }

        VERIFY_ARE_EQUAL(std::wstring(20, L'\u00E0').insert(0, L"\"") + L"\"", Truncated(twenty));
        VERIFY_ARE_EQUAL(std::wstring(19, L'\u00E0').insert(0, L"\"") + L"\u2026\"", Truncated(twenty + accented));
    }

    TEST_METHOD(FormatCommand_WideCharacters_CountedAsTwoColumns)
    {
        // Docker measures display columns, so an East Asian wide character consumes two of the twenty
        // available columns and only ten of them fit.
        const std::string wide = "\xE6\x97\xA5";
        std::string ten;
        for (int i = 0; i < 10; ++i)
        {
            ten += wide;
        }

        VERIFY_ARE_EQUAL(std::wstring(10, L'\u65E5').insert(0, L"\"") + L"\"", Truncated(ten));
        VERIFY_ARE_EQUAL(std::wstring(9, L'\u65E5').insert(0, L"\"") + L"\u2026\"", Truncated(ten + wide));
    }

    TEST_METHOD(FormatCommand_MixedWidthCharacters_AreShortenedByColumn)
    {
        // Two narrow characters leave eighteen columns, so eight wide characters fit before the ellipsis.
        const std::string wide = "\xE6\x97\xA5";
        std::string command = "ab";
        for (int i = 0; i < 10; ++i)
        {
            command += wide;
        }

        VERIFY_ARE_EQUAL(std::wstring{L"\"ab"} + std::wstring(8, L'\u65E5') + L"\u2026\"", Truncated(command));
    }

    TEST_METHOD(FormatCommand_SurrogatePairs_AreNotSplit)
    {
        // Emoji are wide and encoded as surrogate pairs, so a shortened value has to stop on a code point
        // boundary as well as a column boundary.
        const std::string emoji = "\xF0\x9F\x98\x80";
        std::string ten;
        std::wstring expected;
        for (int i = 0; i < 10; ++i)
        {
            ten += emoji;
            expected += L"\U0001F600";
        }

        VERIFY_ARE_EQUAL(L"\"" + expected + L"\"", Truncated(ten));
        VERIFY_ARE_EQUAL(L"\"" + expected.substr(0, 18) + L"\u2026\"", Truncated(ten + emoji));
    }

    TEST_METHOD(FormatCommand_ControlCharacters_AreEscaped)
    {
        // Docker quotes this field with Go's strconv.Quote, which renders control characters as escape sequences
        // rather than emitting them raw and breaking the table row.
        VERIFY_ARE_EQUAL(std::wstring{L"\"line1\\nline2\""}, Truncated("line1\nline2"));
        VERIFY_ARE_EQUAL(std::wstring{L"\"col1\\tcol2\""}, Truncated("col1\tcol2"));
        VERIFY_ARE_EQUAL(std::wstring{L"\"a\\rb\""}, Truncated("a\rb"));
        VERIFY_ARE_EQUAL(std::wstring{L"\"a\\vb\""}, Truncated("a\vb"));
        VERIFY_ARE_EQUAL(std::wstring{L"\"a\\fb\""}, Truncated("a\fb"));
        VERIFY_ARE_EQUAL(std::wstring{L"\"a\\bb\""}, Truncated("a\bb"));
        VERIFY_ARE_EQUAL(std::wstring{L"\"a\\ab\""}, Truncated("a\ab"));
    }

    TEST_METHOD(FormatCommand_NonPrintableCharacters_AreEscapedAsHex)
    {
        // Control characters without a dedicated escape use \x, matching strconv.Quote.
        VERIFY_ARE_EQUAL(
            std::wstring{L"\"a\\x1bb\""},
            Truncated("a\x1b"
                      "b"));
        VERIFY_ARE_EQUAL(
            std::wstring{L"\"a\\x7fb\""},
            Truncated("a\x7f"
                      "b"));
    }

    TEST_METHOD(FormatCommand_PrintableUnicode_IsNotEscaped)
    {
        // Letters and symbols stay verbatim, so only genuinely unprintable values are expanded.
        VERIFY_ARE_EQUAL(std::wstring{L"\"caf\u00E9\""}, Truncated("caf\xC3\xA9"));
    }

    TEST_METHOD(FormatCommand_EscapesAreCountedAfterTruncation)
    {
        // Docker truncates before quoting, so escape expansion does not consume the display budget and the
        // quoted result is wider than the limit.
        VERIFY_ARE_EQUAL(std::wstring{L"\"\\n\\n\\n\\n\\n\""}, Truncated("\n\n\n\n\n"));
    }

    TEST_METHOD(FormatMounts_LongNames_AreShortenedIndependently)
    {
        // Docker shortens every mount name to fifteen columns rather than the list as a whole
        // (ContainerContext.Mounts in cli/command/formatter/container.go).
        const auto mounts = "/var/lib/docker/volumes/data,logs,/mnt/c/users/test/source";
        VERIFY_ARE_EQUAL(std::wstring{L"/var/lib/docke\u2026,logs,/mnt/c/users/t\u2026"}, ContainerService::FormatMounts(mounts, true));
        VERIFY_ARE_EQUAL(wsl::shared::string::MultiByteToWide(mounts), ContainerService::FormatMounts(mounts, false));
    }

    TEST_METHOD(FormatMounts_ShortNames_AreUnchanged)
    {
        VERIFY_ARE_EQUAL(std::wstring{L""}, ContainerService::FormatMounts("", true));
        VERIFY_ARE_EQUAL(std::wstring{L"data-volume"}, ContainerService::FormatMounts("data-volume", true));
        VERIFY_ARE_EQUAL(std::wstring{L"123456789012345"}, ContainerService::FormatMounts("123456789012345", true));
        VERIFY_ARE_EQUAL(std::wstring{L"12345678901234\u2026"}, ContainerService::FormatMounts("1234567890123456", true));
    }

    TEST_METHOD(FormatMounts_WideCharacters_CountedAsTwoColumns)
    {
        // Seven wide characters occupy fourteen columns, so an eighth exceeds the fifteen column budget.
        std::string wide;
        for (int i = 0; i < 8; ++i)
        {
            wide += "\xE6\x97\xA5";
        }

        VERIFY_ARE_EQUAL(std::wstring(7, L'\u65E5') + L"\u2026", ContainerService::FormatMounts(wide, true));
    }

    TEST_METHOD(FormatStatus_RuntimeStatus_IsPreferred)
    {
        VERIFY_ARE_EQUAL(std::wstring{L"Up 5 minutes"}, ContainerService::FormatStatus("Up 5 minutes", WslcContainerStateRunning, 0));
    }

    TEST_METHOD(FormatStatus_EmptyRuntimeStatus_FallsBackToState)
    {
        VERIFY_ARE_EQUAL(std::wstring{L"created"}, ContainerService::FormatStatus("", WslcContainerStateCreated, 0));
    }

    // The fallback is built locally, so json has to render it in invariant English rather than in the
    // machine's display language.
    TEST_METHOD(FormatStatus_EmptyRuntimeStatus_JsonFallbackIsInvariant)
    {
        const auto twoHoursAgo = static_cast<LONGLONG>(std::time(nullptr)) - (2 * 60 * 60);

        VERIFY_ARE_EQUAL(
            std::wstring{L"exited 2 hours ago"},
            ContainerService::FormatStatus("", WslcContainerStateExited, twoHoursAgo, models::FormatType::Json));
        VERIFY_ARE_EQUAL(std::wstring{L"created"}, ContainerService::FormatStatus("", WslcContainerStateCreated, 0, models::FormatType::Json));
    }

    // A status supplied by the runtime is already invariant, so it is passed through unchanged for
    // both formats.
    TEST_METHOD(FormatStatus_RuntimeStatus_IsFormatIndependent)
    {
        VERIFY_ARE_EQUAL(
            std::wstring{L"Up 5 minutes"},
            ContainerService::FormatStatus("Up 5 minutes", WslcContainerStateRunning, 0, models::FormatType::Json));
        VERIFY_ARE_EQUAL(
            std::wstring{L"Up 5 minutes"},
            ContainerService::FormatStatus("Up 5 minutes", WslcContainerStateRunning, 0, models::FormatType::Table));
    }

    TEST_METHOD(FormatHealthStatus_Healthy_IsExtracted)
    {
        VERIFY_ARE_EQUAL(std::string{"healthy"}, ContainerService::FormatHealthStatus("Up 2 minutes (healthy)"));
    }

    TEST_METHOD(FormatHealthStatus_Unhealthy_IsExtracted)
    {
        VERIFY_ARE_EQUAL(std::string{"unhealthy"}, ContainerService::FormatHealthStatus("Up 2 minutes (unhealthy)"));
    }

    TEST_METHOD(FormatHealthStatus_Starting_DropsHealthPrefix)
    {
        VERIFY_ARE_EQUAL(std::string{"starting"}, ContainerService::FormatHealthStatus("Up 2 seconds (health: starting)"));
    }

    TEST_METHOD(FormatHealthStatus_NoHealthCheck_IsEmpty)
    {
        VERIFY_ARE_EQUAL(std::string{}, ContainerService::FormatHealthStatus("Up 2 minutes"));
        VERIFY_ARE_EQUAL(std::string{}, ContainerService::FormatHealthStatus(""));
        VERIFY_ARE_EQUAL(std::string{}, ContainerService::FormatHealthStatus("Created"));
    }

    TEST_METHOD(FormatHealthStatus_NoneIsNotReported)
    {
        VERIFY_ARE_EQUAL(std::string{}, ContainerService::FormatHealthStatus("Up 2 minutes (none)"));
    }

    TEST_METHOD(FormatHealthStatus_UnrelatedParentheses_AreIgnored)
    {
        VERIFY_ARE_EQUAL(std::string{}, ContainerService::FormatHealthStatus("Exited (0) 8 days ago"));
        VERIFY_ARE_EQUAL(std::string{}, ContainerService::FormatHealthStatus("Up 2 minutes (Paused)"));
    }
};

} // namespace WSLCCLIContainerCommandUnitTests
