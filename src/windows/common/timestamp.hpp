/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    timestamp.hpp

Abstract:

    This file contains timestamp and duration helper function declarations.

--*/

#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace wsl::windows::common::timestamp {

// Expands a partial timestamp into a full RFC 3339 one. Hour-only, minute-only and date-only values
// are padded out to a complete time, and a value with no zone designator is resolved against the
// offset currently in effect locally. The input is not validated, so an unrecognized value is
// expanded as-is and left for the parser to reject.
std::string ExpandToRfc3339(const std::string& timestamp);

// Converts an RFC 3339 timestamp to seconds since the unix epoch. Accepts a 'Z' designator or a
// numeric +HH:MM offset, with optional fractional seconds. Timestamps that predate the epoch convert
// to a negative value. Throws E_INVALIDARG if the timestamp is malformed, names an invalid date, or
// has trailing characters.
std::int64_t Rfc3339ToEpoch(const std::string& timestamp);

// Parses a Go duration such as "1h30m", "-1.5h" or "300ms": an optional sign followed by one or more
// decimal values that each carry a unit of ns, us, ms, s, m or h. Returns nothing if the value does
// not match that grammar or overflows.
std::optional<std::chrono::nanoseconds> TryParseDuration(const std::string& duration);

// Renders seconds since the unix epoch in the local time zone, using the layout
// "2006-01-02 15:04:05 -0700 MST". Falls back to UTC when the time zone database is unavailable.
std::string EpochToLocalDisplayTime(LONGLONG timestamp);

// Renders an RFC 3339 timestamp in the same layout, but as UTC and with its fractional seconds
// preserved. An empty input returns an empty string; anything else that cannot be parsed throws.
std::string Rfc3339ToUtcDisplayTime(std::string_view timestamp);

// Renders an elapsed number of seconds as a coarse, localized description such as "About a minute"
// or "3 weeks". Negative values are treated as zero.
std::wstring FormatElapsedSeconds(LONGLONG elapsedSeconds);

// The invariant English form of FormatElapsedSeconds, matching the strings docker produces through
// go-units HumanDuration. Machine readable output uses this so its values do not vary by display
// language.
std::wstring FormatInvariantElapsedSeconds(LONGLONG elapsedSeconds);

// Renders how long ago a timestamp given in seconds since the unix epoch occurred. A timestamp of
// zero means "unset" and returns an empty string.
std::wstring FormatRelativeTime(LONGLONG timestamp);

// The invariant English form of FormatRelativeTime.
std::wstring FormatInvariantRelativeTime(LONGLONG timestamp);

} // namespace wsl::windows::common::timestamp
