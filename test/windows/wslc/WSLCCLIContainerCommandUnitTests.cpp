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
        VERIFY_ARE_EQUAL(std::wstring{LR"("say \"hi\"")"}, Truncated(R"(say "hi")"));
        VERIFY_ARE_EQUAL(std::wstring{LR"("c:\\temp")"}, Truncated(R"(c:\temp)"));
    }

    TEST_METHOD(FormatCommand_MultiByteCharacters_CountedAsSingleCodePoints)
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

    TEST_METHOD(FormatStatus_RuntimeStatus_IsPreferred)
    {
        VERIFY_ARE_EQUAL(std::wstring{L"Up 5 minutes"}, ContainerService::FormatStatus("Up 5 minutes", WslcContainerStateRunning, 0));
    }

    TEST_METHOD(FormatStatus_EmptyRuntimeStatus_FallsBackToState)
    {
        VERIFY_ARE_EQUAL(std::wstring{L"created"}, ContainerService::FormatStatus("", WslcContainerStateCreated, 0));
    }
};

} // namespace WSLCCLIContainerCommandUnitTests
