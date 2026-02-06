// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "pch.h"
#include "DateTime.h"

using namespace std::chrono;

namespace wsl::windows::wslc::util
{
    namespace
    {
        struct OutputTimePointContext
        {
            OutputTimePointContext(std::wostream& stream, const std::chrono::system_clock::time_point& time, TimeFacet facet) :
                Stream(stream), Time(time), Facet(facet)
            {
                auto tt = system_clock::to_time_t(time);
                _localtime64_s(&LocalTime, &tt);
            }

            std::wostream& Stream;
            const std::chrono::system_clock::time_point& Time;
            tm LocalTime{};
            TimeFacet Facet;
        };

        struct OutputTimePointFacetInfo
        {
            TimeFacet Facet;
            wchar_t FollowingSeparator;
            void (*Action)(const OutputTimePointContext&);
        };
    }

    void OutputTimePoint(std::wostream& stream, const std::chrono::system_clock::time_point& time, bool useRFC3339)
    {
        OutputTimePoint(stream, time, TimeFacet::Default | (useRFC3339 ? TimeFacet::RFC3339 : TimeFacet::None));
    }

    // If moved to C++20, this can be replaced with standard library implementations.
    void OutputTimePoint(std::wostream& stream, const std::chrono::system_clock::time_point& time, TimeFacet facet)
    {
        OutputTimePointContext context{ stream, time, facet };
        using Ctx = const OutputTimePointContext&;

        bool useRFC3339 = WI_IsFlagSet(facet, TimeFacet::RFC3339);
        bool filename = WI_IsFlagSet(facet, TimeFacet::Filename);
        wchar_t day_time_separator = useRFC3339 ? L'T' : (filename ? L'-' : L' ');
        wchar_t time_field_separator = filename ? L'-' : L':';

        bool needsSeparator = false;
        wchar_t currentSeparator = L'-';

        for (const auto& info : {
            OutputTimePointFacetInfo{ TimeFacet::ShortYear, '-', [](Ctx ctx) { ctx.Stream << (ctx.LocalTime.tm_year - 100); }},
            OutputTimePointFacetInfo{ TimeFacet::Year, '-', [](Ctx ctx) { ctx.Stream << (1900 + ctx.LocalTime.tm_year); }},
            OutputTimePointFacetInfo{ TimeFacet::Month, '-', [](Ctx ctx) { ctx.Stream << std::setw(2) << std::setfill(L'0') << (1 + ctx.LocalTime.tm_mon); }},
            OutputTimePointFacetInfo{ TimeFacet::Day, day_time_separator, [](Ctx ctx) { ctx.Stream << std::setw(2) << std::setfill(L'0') << ctx.LocalTime.tm_mday; }},
            OutputTimePointFacetInfo{ TimeFacet::Hour, time_field_separator, [](Ctx ctx) { ctx.Stream << std::setw(2) << std::setfill(L'0') << ctx.LocalTime.tm_hour; }},
            OutputTimePointFacetInfo{ TimeFacet::Minute, time_field_separator, [](Ctx ctx) { ctx.Stream << std::setw(2) << std::setfill(L'0') << ctx.LocalTime.tm_min; }},
            OutputTimePointFacetInfo{ TimeFacet::Second, '.', [](Ctx ctx) { ctx.Stream << std::setw(2) << std::setfill(L'0') << ctx.LocalTime.tm_sec; }},
            OutputTimePointFacetInfo{ TimeFacet::Millisecond, '-', [](Ctx ctx)
            {
                // Get partial seconds
                auto sinceEpoch = ctx.Time.time_since_epoch();
                auto leftoverMillis = duration_cast<milliseconds>(sinceEpoch) - duration_cast<seconds>(sinceEpoch);

                ctx.Stream << std::setw(3) << std::setfill(L'0') << leftoverMillis.count();
            }},
            OutputTimePointFacetInfo{ TimeFacet::RFC3339, '\0', [](Ctx ctx)
            {
                // RFC 3339 requires adding time zone info.
                // No need to bother getting the actual time zone as we don't need it.
                // -00:00 represents an unspecified time zone, not UTC.
                ctx.Stream << "00:00";
            }},
            })
        {
            if (WI_AreAllFlagsSet(facet, info.Facet))
            {
                if (needsSeparator)
                {
                    stream << currentSeparator;
                }

                info.Action(context);
                needsSeparator = true;
            }

            // Getting this right for every mix of facets is probably not possible.
            // Future needs can dictate changes here.
            currentSeparator = info.FollowingSeparator;
        }
    }

    std::wstring TimePointToString(const std::chrono::system_clock::time_point& time, bool useRFC3339)
    {
        std::wostringstream stream;
        OutputTimePoint(stream, time, useRFC3339);
        return std::move(stream).str();
    }

    std::wstring TimePointToString(const std::chrono::system_clock::time_point& time, TimeFacet facet)
    {
        std::wostringstream stream;
        OutputTimePoint(stream, time, facet);
        return std::move(stream).str();
    }

    std::wstring GetCurrentTimeForFilename(bool shortTime)
    {
        return TimePointToString(std::chrono::system_clock::now(), (shortTime ? TimeFacet::ShortYearSecondPrecision : TimeFacet::Default) | TimeFacet::Filename);
    }

    int64_t GetCurrentUnixEpoch()
    {
        static_assert(std::is_same_v<int64_t, decltype(time(nullptr))>, "time returns a 64-bit integer");
        time_t now = time(nullptr);
        return static_cast<int64_t>(now);
    }

    int64_t ConvertSystemClockToUnixEpoch(const std::chrono::system_clock::time_point& time)
    {
        static_assert(std::is_same_v<int64_t, decltype(std::chrono::system_clock::to_time_t(time))>, "to_time_t returns a 64-bit integer");
        time_t timeAsTimeT = std::chrono::system_clock::to_time_t(time);
        return static_cast<int64_t>(timeAsTimeT);
    }

    std::chrono::system_clock::time_point ConvertUnixEpochToSystemClock(int64_t epoch)
    {
        return std::chrono::system_clock::from_time_t(static_cast<time_t>(epoch));
    }

    std::chrono::system_clock::time_point ConvertFiletimeToSystemClock(const FILETIME& fileTime)
    {
        return winrt::clock::to_sys(winrt::clock::from_FILETIME(fileTime));
    }
}
