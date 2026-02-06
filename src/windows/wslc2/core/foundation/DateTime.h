// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include <chrono>
#include <ostream>

namespace wsl::windows::wslc::util
{
    // The individual aspects of a time point.
    enum class TimeFacet
    {
        None        = 0x000,
        Millisecond = 0x001,
        Second      = 0x002,
        Minute      = 0x004,
        Hour        = 0x008,
        Day         = 0x010,
        Month       = 0x020,
        Year        = 0x040,
        // `Year - 2000` [2 digits for 75 more years]
        ShortYear   = 0x080,
        // Includes unspecified time zone
        RFC3339     = 0x100,
        // Limits special character use
        Filename    = 0x200,

        Default = Year | Month | Day | Hour | Minute | Second | Millisecond,
        ShortYearSecondPrecision = ShortYear | Month | Day | Hour | Minute | Second,
    };

    DEFINE_ENUM_FLAG_OPERATORS(TimeFacet);

    // Writes the given time to the given stream.
    // Assumes that system_clock uses Linux epoch (as required by C++20 standard).
    // Time is also assumed to be after the epoch.
    void OutputTimePoint(std::wostream& stream, const std::chrono::system_clock::time_point& time, bool useRFC3339 = false);
    void OutputTimePoint(std::wostream& stream, const std::chrono::system_clock::time_point& time, TimeFacet facet);

    // Converts the time point to a string using OutputTimePoint.
    std::wstring TimePointToString(const std::chrono::system_clock::time_point& time, bool useRFC3339 = false);
    std::wstring TimePointToString(const std::chrono::system_clock::time_point& time, TimeFacet facet);

    // Gets the current time as a string. Can be used as a file name.
    // Tries to make things a little bit shorter when shortTime == true.
    std::wstring GetCurrentTimeForFilename(bool shortTime = false);

    // Gets the current time as a unix epoch value.
    int64_t GetCurrentUnixEpoch();

    // Converts the given unix epoch time to a system_clock::time_point.
    int64_t ConvertSystemClockToUnixEpoch(const std::chrono::system_clock::time_point& time);

    // Converts the given unix epoch time to a system_clock::time_point.
    std::chrono::system_clock::time_point ConvertUnixEpochToSystemClock(int64_t epoch);

    // Converts the given file time to a system_clock::time_point.
    std::chrono::system_clock::time_point ConvertFiletimeToSystemClock(const FILETIME& fileTime);
}
