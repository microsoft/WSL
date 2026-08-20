/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    string.hpp

Abstract:

    This file contains string management function declarations.

--*/

#pragma once

#include "helpers.hpp"
#include "stringshared.h"
#include <chrono>
#include <optional>

// Forward declare types to avoid pulling in excessive number of headers.
using IP_ADDRESS_PREFIX = struct _IP_ADDRESS_PREFIX;
using SOCKADDR_INET = union _SOCKADDR_INET;

namespace wsl::windows::common::string {

enum class StorageSizeUnit
{
    Decimal,
    Binary
};

std::optional<uint64_t> ParseStorageSize(std::wstring_view String, StorageSizeUnit Unit);

std::wstring FormatStorageSize(uint64_t Bytes, StorageSizeUnit Unit, uint32_t DecimalPlaces, bool IncludeSpace = false);

std::wstring FormatBytes(uint64_t Bytes);

// Formats a size the way docker reports image sizes: base 1000, three significant digits and no
// space (119856765 -> "120MB").
std::wstring FormatDockerSize(uint64_t Bytes);

std::vector<std::string> InitializeStringSet(_In_count_(BufferSize) LPCSTR Buffer, _In_ SIZE_T BufferSize);

bool IsPathComponentEqual(const std::wstring_view String1, const std::wstring_view String2);

std::wstring MultiByteToWide(_In_ LPCSTR Source, _In_ size_t CharacterCount = -1);

std::wstring MultiByteToWide(_In_ std::string_view Source);

std::wstring_view StripLeadingWhitespace(_In_ std::wstring_view String);

std::wstring_view StripQuotes(_In_ std::wstring_view String);

std::string IpPrefixAddressToString(const IP_ADDRESS_PREFIX& ipAddressPrefix);
std::string SockAddrInetToString(const SOCKADDR_INET& sockAddrInet);
std::wstring SockAddrInetToWstring(const SOCKADDR_INET& sockAddrInet);
std::wstring IntegerIpv4ToWstring(const uint32_t ipAddress);
SOCKADDR_INET StringToSockAddrInet(const std::wstring& stringIpAddress);
std::wstring BytesToHex(const std::vector<BYTE>& bytes);
std::vector<BYTE> HexToBytes(std::string_view input);
std::vector<BYTE> HexToBytes(std::wstring_view input);

std::string WideToMultiByte(_In_opt_ LPCWSTR Source, _In_ size_t CharacterCount = -1);

std::string WideToMultiByte(_In_ std::wstring_view Source);

std::wstring TruncateId(_In_ std::wstring_view id, bool shortenLength = true);
std::string TruncateId(_In_ std::string_view id, bool shortenLength = true);

// Expands a partial timestamp into a full RFC 3339 one. Hour-only, minute-only and date-only values
// are padded out to a complete time, and a value with no zone designator is resolved against the
// offset currently in effect locally. Input that is not recognized is returned for the parser to
// reject.
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

// Renders how long ago a timestamp given in seconds since the unix epoch occurred. A timestamp of
// zero means "unset" and returns an empty string.
std::wstring FormatRelativeTime(LONGLONG timestamp);

// Template implementation for TruncateId to avoid code duplication.
// Algorithm inspired from Moby for consistency in presentation of shortened IDs.
// Always strips the algorithm prefix (e.g., "sha256:") if present, and optionally shortens to 12 characters.
template <typename TChar>
inline std::basic_string<TChar> TruncateIdImpl(std::basic_string_view<TChar> id, bool shortenLength)
{
    constexpr size_t shortLen = 12;
    constexpr TChar colon = TChar(':');

    // Find and skip algorithm prefix if present (e.g., "sha256:")
    auto colonPos = id.find(colon);
    if (colonPos != std::basic_string_view<TChar>::npos)
    {
        id.remove_prefix(colonPos + 1);
    }

    if (shortenLength && id.length() > shortLen)
    {
        return std::basic_string<TChar>{id.substr(0, shortLen)};
    }

    return std::basic_string<TChar>{id};
}

struct PhysicalMacAddress
{
    BYTE Address[MAX_ADAPTER_ADDRESS_LENGTH]{};
};

} // namespace wsl::windows::common::string
