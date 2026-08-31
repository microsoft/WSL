/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    timestamp.cpp

Abstract:

    This file contains timestamp and duration helper function definitions.

--*/

#include "precomp.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>

static std::string LocalUtcOffset()
{
    try
    {
        const auto* zone = std::chrono::current_zone();
        const auto offset = zone->get_info(std::chrono::system_clock::now()).offset;
        const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(offset).count();
        const auto magnitude = std::abs(minutes);

        return std::format("{}{:02}:{:02}", minutes < 0 ? '-' : '+', magnitude / 60, magnitude % 60);
    }
    catch (...)
    {
        // The time zone database is unavailable, so fall back to UTC rather than failing the caller.
        LOG_CAUGHT_EXCEPTION();
        return "+00:00";
    }
}

std::string wsl::windows::common::timestamp::ExpandToRfc3339(const std::string& timestamp)
{
    std::string_view value{timestamp};
    std::string_view zone;

    if (!value.empty() && (value.back() == 'Z' || value.back() == 'z'))
    {
        zone = value.substr(value.size() - 1);
        value.remove_suffix(1);
    }
    else if (const auto plus = value.find('+'); plus != std::string_view::npos)
    {
        zone = value.substr(plus);
        value = value.substr(0, plus);
    }
    else if (std::ranges::count(value, '-') == 3)
    {
        // The first two dashes belong to the date, so a third one opens a negative zone offset.
        auto offset = value.find('-');
        offset = value.find('-', offset + 1);
        offset = value.find('-', offset + 1);

        zone = value.substr(offset);
        value = value.substr(0, offset);
    }

    const auto separator = value.find('T');
    const auto time = separator == std::string_view::npos ? std::string_view{} : value.substr(separator + 1);

    std::string expanded{value.substr(0, separator)};
    expanded += 'T';

    if (time.empty())
    {
        expanded += "00:00:00";
    }
    else
    {
        expanded += time;

        // Pad an hour-only or minute-only time out to a full HH:MM:SS.
        const auto colons = std::ranges::count(time, ':');
        if (colons == 0)
        {
            expanded += ":00:00";
        }
        else if (colons == 1)
        {
            expanded += ":00";
        }
    }

    expanded += zone.empty() ? LocalUtcOffset() : std::string{zone};

    return expanded;
}

std::int64_t wsl::windows::common::timestamp::Rfc3339ToEpoch(const std::string& timestamp)
{
    // Normalize a trailing 'Z' or 'z' to '+00:00' so that %Ez parses every zone uniformly.
    std::string normalized{timestamp};
    if (!normalized.empty() && (normalized.back() == 'Z' || normalized.back() == 'z'))
    {
        normalized.pop_back();
        normalized += "+00:00";
    }

    // Strip any fractional seconds. The value is truncated to whole seconds regardless, and parsing at
    // second precision keeps timestamps outside the nanosecond range from silently wrapping.
    const auto separator = normalized.find('.');
    if (separator != std::string::npos)
    {
        auto end = separator + 1;
        while (end < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[end])) != 0)
        {
            ++end;
        }

        // A separator with no fractional digits is invalid, but std::chrono::parse otherwise accepts it.
        THROW_HR_IF_MSG(E_INVALIDARG, end == separator + 1, "Failed to parse timestamp '%hs'", timestamp.c_str());

        normalized.erase(separator, end - separator);
    }

    // Validate the day up front since std::chrono::parse silently wraps invalid dates (e.g. Feb 31 -> Mar 2).
    if (normalized.size() >= 10 && normalized[4] == '-' && normalized[7] == '-')
    {
        int year{};
        int month{};
        int day{};
        const auto yearResult = std::from_chars(normalized.data(), normalized.data() + 4, year);
        const auto monthResult = std::from_chars(normalized.data() + 5, normalized.data() + 7, month);
        const auto dayResult = std::from_chars(normalized.data() + 8, normalized.data() + 10, day);
        if (yearResult.ec == std::errc() && monthResult.ec == std::errc() && dayResult.ec == std::errc())
        {
            const auto date = std::chrono::year{year} / std::chrono::month{static_cast<unsigned>(month)} /
                              std::chrono::day{static_cast<unsigned>(day)};

            THROW_HR_IF_MSG(E_INVALIDARG, !date.ok(), "Failed to parse timestamp '%hs'", timestamp.c_str());
        }
    }

    std::chrono::sys_seconds parsed{};
    std::istringstream stream(normalized);
    stream >> std::chrono::parse("%FT%T%Ez", parsed);
    THROW_HR_IF_MSG(E_INVALIDARG, stream.fail(), "Failed to parse timestamp '%hs'", timestamp.c_str());
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        stream.peek() != std::istringstream::traits_type::eof(),
        "Unexpected trailing characters in timestamp '%hs'",
        timestamp.c_str());

    return parsed.time_since_epoch().count();
}

std::optional<std::chrono::nanoseconds> wsl::windows::common::timestamp::TryParseDuration(const std::string& duration)
{
    if (duration.empty())
    {
        return std::nullopt;
    }

    size_t pos = 0;
    bool negative = false;
    if (duration[pos] == '+' || duration[pos] == '-')
    {
        negative = duration[pos] == '-';
        pos++;
    }

    // Special case: a bare "0" (with optional sign) is a valid zero duration.
    if (duration.substr(pos) == "0")
    {
        return std::chrono::nanoseconds{0};
    }

    // Accumulate in a long double so fractional units (e.g. "1.5h") are handled, then round.
    long double totalNanos = 0.0L;
    bool sawValue = false;

    while (pos < duration.size())
    {
        // Parse the numeric part (integer and/or fraction).
        const size_t numberStart = pos;
        while (pos < duration.size() && (std::isdigit(static_cast<unsigned char>(duration[pos])) || duration[pos] == '.'))
        {
            pos++;
        }

        const std::string numberStr = duration.substr(numberStart, pos - numberStart);
        if (numberStr.empty() || numberStr == "." || std::count(numberStr.begin(), numberStr.end(), '.') > 1)
        {
            return std::nullopt;
        }

        // Parse the unit (everything up to the next digit or '.').
        const size_t unitStart = pos;
        while (pos < duration.size() && !std::isdigit(static_cast<unsigned char>(duration[pos])) && duration[pos] != '.')
        {
            pos++;
        }

        const std::string unit = duration.substr(unitStart, pos - unitStart);

        long double multiplier{};
        if (unit == "ns")
        {
            multiplier = 1.0L;
        }
        else if (unit == "us" || unit == "\xC2\xB5s" /* µs (U+00B5) */ || unit == "\xCE\xBCs" /* μs (U+03BC) */)
        {
            multiplier = 1000L;
        }
        else if (unit == "ms")
        {
            multiplier = 1000000L;
        }
        else if (unit == "s")
        {
            multiplier = 1000000000L;
        }
        else if (unit == "m")
        {
            multiplier = 60000000000L;
        }
        else if (unit == "h")
        {
            multiplier = 3600000000000L;
        }
        else
        {
            return std::nullopt;
        }

        long double value{};
        auto [ptr, ec] = std::from_chars(numberStr.data(), numberStr.data() + numberStr.size(), value, std::chars_format::fixed);
        if (ptr != numberStr.data() + numberStr.size() || ec != std::errc())
        {
            return std::nullopt;
        }

        totalNanos += value * multiplier;
        sawValue = true;
    }

    if (!sawValue)
    {
        return std::nullopt;
    }

    if (negative)
    {
        totalNanos = -totalNanos;
    }

    if (totalNanos > static_cast<long double>(std::numeric_limits<int64_t>::max()) ||
        totalNanos < static_cast<long double>(std::numeric_limits<int64_t>::min()))
    {
        return std::nullopt;
    }

    return std::chrono::nanoseconds{static_cast<int64_t>(std::llroundl(totalNanos))};
}

std::string wsl::windows::common::timestamp::EpochToLocalDisplayTime(LONGLONG timestamp)
{
    const auto time =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::from_time_t(static_cast<std::time_t>(timestamp)));

    try
    {
        const auto* zone = std::chrono::current_zone();
        return std::format("{:%F %T %z} {}", std::chrono::zoned_time{zone, time}, zone->get_info(time).abbrev);
    }
    catch (...)
    {
        // The time zone database is unavailable, so report UTC rather than failing the caller.
        LOG_CAUGHT_EXCEPTION();
        return std::format("{:%F %T} +0000 UTC", time);
    }
}

std::string wsl::windows::common::timestamp::Rfc3339ToUtcDisplayTime(std::string_view timestamp)
{
    if (timestamp.empty())
    {
        return {};
    }

    // Fractional digits are dropped by the parse and vary in length, so they are captured verbatim
    // and re-inserted after formatting.
    std::string fraction;
    const auto separator = timestamp.find('.');
    if (separator != std::string_view::npos)
    {
        auto end = separator + 1;
        while (end < timestamp.size() && (std::isdigit(static_cast<unsigned char>(timestamp[end])) != 0))
        {
            end++;
        }

        fraction = timestamp.substr(separator, end - separator);
    }

    const std::chrono::sys_seconds parsed{std::chrono::seconds{Rfc3339ToEpoch(std::string{timestamp})}};

    // Network timestamps are reported in UTC rather than the local time zone.
    return std::format("{:%F %T}{} +0000 UTC", parsed, fraction);
}

namespace {

enum class ElapsedUnit
{
    LessThanASecond,
    OneSecond,
    Seconds,
    AboutAMinute,
    Minutes,
    AboutAnHour,
    Hours,
    Days,
    Weeks,
    Months,
    Years,
};

struct ElapsedDuration
{
    ElapsedUnit Unit;
    LONGLONG Count;
};

} // namespace

// Buckets an elapsed duration using the thresholds docker applies in go-units HumanDuration. The
// localized and invariant renderings share this so the two can only differ in wording.
static ElapsedDuration ClassifyElapsedSeconds(LONGLONG elapsedSeconds)
{
    using namespace std::chrono_literals;

    constexpr LONGLONG SecondsPerMinute = std::chrono::duration_cast<std::chrono::seconds>(1min).count();
    constexpr LONGLONG SecondsPerHour = std::chrono::duration_cast<std::chrono::seconds>(1h).count();
    constexpr LONGLONG HoursPerDay = 24;
    constexpr LONGLONG MinutesPerHour = 60;

    const auto elapsed = std::max<LONGLONG>(elapsedSeconds, 0);

    if (elapsed < 1)
    {
        return {ElapsedUnit::LessThanASecond, 0};
    }
    else if (elapsed == 1)
    {
        return {ElapsedUnit::OneSecond, 1};
    }
    else if (elapsed < SecondsPerMinute)
    {
        return {ElapsedUnit::Seconds, elapsed};
    }

    const auto minutes = elapsed / SecondsPerMinute;
    if (minutes == 1)
    {
        return {ElapsedUnit::AboutAMinute, 1};
    }
    else if (minutes < MinutesPerHour)
    {
        return {ElapsedUnit::Minutes, minutes};
    }

    // Rounded to the nearest hour rather than truncated.
    const auto hours = (elapsed + (SecondsPerHour / 2)) / SecondsPerHour;
    if (hours == 1)
    {
        return {ElapsedUnit::AboutAnHour, 1};
    }
    else if (hours < HoursPerDay * 2)
    {
        return {ElapsedUnit::Hours, hours};
    }
    else if (hours < HoursPerDay * 7 * 2)
    {
        return {ElapsedUnit::Days, hours / HoursPerDay};
    }
    else if (hours < HoursPerDay * 30 * 2)
    {
        return {ElapsedUnit::Weeks, hours / HoursPerDay / 7};
    }
    else if (hours < HoursPerDay * 365 * 2)
    {
        return {ElapsedUnit::Months, hours / HoursPerDay / 30};
    }

    return {ElapsedUnit::Years, elapsed / SecondsPerHour / HoursPerDay / 365};
}

std::wstring wsl::windows::common::timestamp::FormatElapsedSeconds(LONGLONG elapsedSeconds)
{
    using wsl::shared::Localization;

    const auto [unit, count] = ClassifyElapsedSeconds(elapsedSeconds);
    switch (unit)
    {
    case ElapsedUnit::LessThanASecond:
        return Localization::WSLCCLI_RelativeTimeLessThanASecond();
    case ElapsedUnit::OneSecond:
        return Localization::WSLCCLI_RelativeTimeOneSecond();
    case ElapsedUnit::Seconds:
        return Localization::WSLCCLI_RelativeTimeSeconds(count);
    case ElapsedUnit::AboutAMinute:
        return Localization::WSLCCLI_RelativeTimeAboutAMinute();
    case ElapsedUnit::Minutes:
        return Localization::WSLCCLI_RelativeTimeMinutes(count);
    case ElapsedUnit::AboutAnHour:
        return Localization::WSLCCLI_RelativeTimeAboutAnHour();
    case ElapsedUnit::Hours:
        return Localization::WSLCCLI_RelativeTimeHours(count);
    case ElapsedUnit::Days:
        return Localization::WSLCCLI_RelativeTimeDays(count);
    case ElapsedUnit::Weeks:
        return Localization::WSLCCLI_RelativeTimeWeeks(count);
    case ElapsedUnit::Months:
        return Localization::WSLCCLI_RelativeTimeMonths(count);
    case ElapsedUnit::Years:
        return Localization::WSLCCLI_RelativeTimeYears(count);
    default:
        THROW_HR(E_UNEXPECTED);
    }
}

std::wstring wsl::windows::common::timestamp::FormatInvariantElapsedSeconds(LONGLONG elapsedSeconds)
{
    const auto [unit, count] = ClassifyElapsedSeconds(elapsedSeconds);
    switch (unit)
    {
    case ElapsedUnit::LessThanASecond:
        return L"Less than a second ago";
    case ElapsedUnit::OneSecond:
        return L"1 second ago";
    case ElapsedUnit::Seconds:
        return std::format(L"{} seconds ago", count);
    case ElapsedUnit::AboutAMinute:
        return L"About a minute ago";
    case ElapsedUnit::Minutes:
        return std::format(L"{} minutes ago", count);
    case ElapsedUnit::AboutAnHour:
        return L"About an hour ago";
    case ElapsedUnit::Hours:
        return std::format(L"{} hours ago", count);
    case ElapsedUnit::Days:
        return std::format(L"{} days ago", count);
    case ElapsedUnit::Weeks:
        return std::format(L"{} weeks ago", count);
    case ElapsedUnit::Months:
        return std::format(L"{} months ago", count);
    case ElapsedUnit::Years:
        return std::format(L"{} years ago", count);
    default:
        THROW_HR(E_UNEXPECTED);
    }
}

std::wstring wsl::windows::common::timestamp::FormatRelativeTime(LONGLONG timestamp)
{
    if (timestamp == 0)
    {
        return {};
    }

    return FormatElapsedSeconds(static_cast<LONGLONG>(std::time(nullptr)) - timestamp);
}

std::wstring wsl::windows::common::timestamp::FormatInvariantRelativeTime(LONGLONG timestamp)
{
    if (timestamp == 0)
    {
        return {};
    }

    return FormatInvariantElapsedSeconds(static_cast<LONGLONG>(std::time(nullptr)) - timestamp);
}
