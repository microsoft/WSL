// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCCLITestHelpers.h"

#include "timestamp.hpp"

using namespace wsl::shared;
using namespace wsl::windows::common::timestamp;
using namespace WSLCTestHelpers;
using namespace WEX::Logging;
using namespace WEX::Common;
using namespace WEX::TestExecution;

namespace WSLCCLIRelativeTimeUnitTests {

class WSLCCLIRelativeTimeUnitTests
{
    WSLC_TEST_CLASS(WSLCCLIRelativeTimeUnitTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        return true;
    }

    static std::wstring FormatElapsed(LONGLONG secondsAgo)
    {
        return FormatElapsedSeconds(secondsAgo);
    }

    TEST_METHOD(RelativeTime_ZeroTimestamp_ReturnsEmpty)
    {
        VERIFY_ARE_EQUAL(std::wstring{}, FormatRelativeTime(0));
    }

    TEST_METHOD(RelativeTime_NegativeElapsed_ClampsToLessThanASecond)
    {
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeLessThanASecond(), FormatElapsedSeconds(-600));
    }

    TEST_METHOD(RelativeTime_Seconds)
    {
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeLessThanASecond(), FormatElapsed(0));
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeOneSecond(), FormatElapsed(1));
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeSeconds(2), FormatElapsed(2));
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeSeconds(59), FormatElapsed(59));
    }

    TEST_METHOD(RelativeTime_Minutes)
    {
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeAboutAMinute(), FormatElapsed(60));
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeAboutAMinute(), FormatElapsed(119));
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeMinutes(2), FormatElapsed(120));
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeMinutes(59), FormatElapsed(59 * 60));
    }

    TEST_METHOD(RelativeTime_Hours)
    {
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeAboutAnHour(), FormatElapsed(60 * 60));
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeHours(2), FormatElapsed(2 * 60 * 60));
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeHours(47), FormatElapsed(47 * 60 * 60));
    }

    // The hour count is rounded to the nearest hour rather than truncated.
    TEST_METHOD(RelativeTime_HoursAreRounded)
    {
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeAboutAnHour(), FormatElapsed(89 * 60));
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeHours(2), FormatElapsed(90 * 60));
    }

    TEST_METHOD(RelativeTime_Days)
    {
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeDays(2), FormatElapsed(48 * 60 * 60));
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeDays(13), FormatElapsed(13 * 24 * 60 * 60));
    }

    TEST_METHOD(RelativeTime_Weeks)
    {
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeWeeks(2), FormatElapsed(14 * 24 * 60 * 60));
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeWeeks(8), FormatElapsed(59 * 24 * 60 * 60));
    }

    TEST_METHOD(RelativeTime_Months)
    {
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeMonths(2), FormatElapsed(60 * 24 * 60 * 60));
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeMonths(12), FormatElapsed(365 * 24 * 60 * 60));
    }

    TEST_METHOD(RelativeTime_Years)
    {
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeYears(2), FormatElapsed(730 * 24 * 60 * 60LL));
        VERIFY_ARE_EQUAL(Localization::WSLCCLI_RelativeTimeYears(3), FormatElapsed(3 * 365 * 24 * 60 * 60LL));
    }
};

} // namespace WSLCCLIRelativeTimeUnitTests
