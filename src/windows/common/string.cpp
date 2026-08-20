/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    string.cpp

Abstract:

    This file contains string helper function definitions.

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

std::vector<std::string> wsl::windows::common::string::InitializeStringSet(_In_count_(BufferSize) LPCSTR Buffer, _In_ SIZE_T BufferSize)
{
    // Ensure the buffer ends with two NULL terminators.
    THROW_HR_IF(E_INVALIDARG, ((BufferSize < 2) || (Buffer[BufferSize - 1] != ANSI_NULL) || (Buffer[BufferSize - 2] != ANSI_NULL)));

    std::vector<std::string> values{};
    for (LPCSTR current = Buffer; ANSI_NULL != *current; current += strlen(current) + 1)
    {
        values.push_back(current);
    }

    return values;
}

bool wsl::windows::common::string::IsPathComponentEqual(const std::wstring_view String1, const std::wstring_view String2)
{
    return CompareStringOrdinal(String1.data(), static_cast<int>(String1.size()), String2.data(), static_cast<int>(String2.size()), true) == CSTR_EQUAL;
}

std::wstring wsl::windows::common::string::MultiByteToWide(_In_ LPCSTR Source, _In_ size_t CharacterCount)
{
    if (CharacterCount == -1)
    {
        CharacterCount = Source ? strlen(Source) : 0;
    }

    if (CharacterCount == 0)
    {
        return {};
    }

    THROW_HR_IF(E_BOUNDS, (CharacterCount > static_cast<size_t>(std::numeric_limits<int>::max())));

    int required = MultiByteToWideChar(CP_UTF8, 0, Source, gsl::narrow_cast<int>(CharacterCount), nullptr, 0);
    THROW_LAST_ERROR_IF(required == 0);

    std::wstring converted(required, L'\0');
    required = MultiByteToWideChar(CP_UTF8, 0, Source, gsl::narrow_cast<int>(CharacterCount), converted.data(), required);
    THROW_LAST_ERROR_IF(required == 0);

    return converted;
}

std::wstring wsl::windows::common::string::MultiByteToWide(_In_ std::string_view Source)
{
    return MultiByteToWide(Source.data(), Source.size());
}

std::wstring_view wsl::windows::common::string::StripLeadingWhitespace(_In_ std::wstring_view String)
{
    const size_t Index = String.find_first_not_of(L" \t");
    if (Index != std::wstring_view::npos)
    {
        String.remove_prefix(Index);
    }
    else
    {
        String = {};
    }

    return String;
}

std::wstring_view wsl::windows::common::string::StripQuotes(_In_ std::wstring_view String)
{
    // If the string begins and ends with a quote character, remove them.
    std::wstring_view Stripped = String;
    if ((Stripped.size() > 1) && (Stripped[0] == L'\"') && (Stripped[Stripped.size() - 1] == L'\"'))
    {
        Stripped.remove_prefix(1);
        Stripped.remove_suffix(1);
    }

    return Stripped;
}

std::string wsl::windows::common::string::IpPrefixAddressToString(const IP_ADDRESS_PREFIX& ipAddressPrefix)
{
    return std::format("{}/{}", SockAddrInetToString(ipAddressPrefix.Prefix), static_cast<uint32_t>(ipAddressPrefix.PrefixLength));
}

std::string wsl::windows::common::string::SockAddrInetToString(const SOCKADDR_INET& sockAddrInet)
{
    std::string ipAddress(INET6_ADDRSTRLEN, '\0');
    switch (sockAddrInet.si_family)
    {
    case AF_INET:
        RtlIpv4AddressToStringA(&sockAddrInet.Ipv4.sin_addr, ipAddress.data());
        break;
    case AF_INET6:
        RtlIpv6AddressToStringA(&sockAddrInet.Ipv6.sin6_addr, ipAddress.data());
        break;
    default:
        ipAddress = std::format("[[ADDRESS_FAMILY {}]]", sockAddrInet.si_family);
        break;
    }
    ipAddress.resize(std::strlen(ipAddress.data()));
    return ipAddress;
}

std::wstring wsl::windows::common::string::SockAddrInetToWstring(const SOCKADDR_INET& sockAddrInet)
{
    std::wstring ipAddress(INET6_ADDRSTRLEN, '\0');
    switch (sockAddrInet.si_family)
    {
    case AF_INET:
        RtlIpv4AddressToStringW(&sockAddrInet.Ipv4.sin_addr, ipAddress.data());
        break;
    case AF_INET6:
        RtlIpv6AddressToStringW(&sockAddrInet.Ipv6.sin6_addr, ipAddress.data());
        break;
    default:
        ipAddress = std::format(L"[[ADDRESS_FAMILY {}]]", sockAddrInet.si_family);
        break;
    }
    ipAddress.resize(std::wcslen(ipAddress.data()));
    return ipAddress;
}

std::wstring wsl::windows::common::string::IntegerIpv4ToWstring(const uint32_t ipAddress)
{
    in_addr address{};
    address.S_un.S_addr = ipAddress;

    std::wstring stringAddress(INET_ADDRSTRLEN, '\0');
    WI_VERIFY(InetNtopW(AF_INET, &address, stringAddress.data(), stringAddress.size()) != nullptr);
    stringAddress.resize(wcslen(stringAddress.c_str()));

    return stringAddress;
}

SOCKADDR_INET wsl::windows::common::string::StringToSockAddrInet(const std::wstring& stringIpAddress)
{
    SOCKADDR_INET returnSockaddr{};
    if (stringIpAddress.empty())
    {
        // return an empty IPv4 sockaddr
        returnSockaddr.si_family = AF_INET;
    }
    else if (stringIpAddress.find(':', 0) == std::string::npos)
    {
        returnSockaddr.si_family = AF_INET;
        const wchar_t* terminator;
        THROW_IF_WIN32_ERROR_MSG(
            RtlIpv4StringToAddressW(stringIpAddress.c_str(), TRUE, &terminator, &returnSockaddr.Ipv4.sin_addr),
            "RtlIpv4StringToAddressW(%ws)",
            stringIpAddress.c_str());
    }
    else
    {
        returnSockaddr.si_family = AF_INET6;
        const wchar_t* terminator;
        THROW_IF_WIN32_ERROR_MSG(
            RtlIpv6StringToAddressW(stringIpAddress.c_str(), &terminator, &returnSockaddr.Ipv6.sin6_addr),
            "RtlIpv6StringToAddressW(%ws)",
            stringIpAddress.c_str());
    }

    return returnSockaddr;
}

std::wstring wsl::windows::common::string::BytesToHex(const std::vector<BYTE>& bytes)
{
    std::wstringstream str;

    str << L"0x";
    str << std::hex;

    for (const auto e : bytes)
    {
        str << std::setw(2) << std::setfill(L'0') << static_cast<int>(e);
    }

    return str.str();
}

namespace {
bool IsHexSpecifier(char first, char second)
{
    return first == '0' && tolower(static_cast<unsigned char>(second)) == 'x';
}

bool IsHexSpecifier(wchar_t first, wchar_t second)
{
    return first == L'0' && towlower(second) == L'x';
}

BYTE ConvertHexByte(const char* hex, char** endPtr)
{
    return static_cast<BYTE>(strtoul(hex, endPtr, 16));
}

BYTE ConvertHexByte(const wchar_t* hex, wchar_t** endPtr)
{
    return static_cast<BYTE>(wcstoul(hex, endPtr, 16));
}

template <typename T>
std::vector<BYTE> HexToBytesT(std::basic_string_view<T> input)
{
    if (input.length() % 2 != 0)
    {
        THROW_HR_WITH_USER_ERROR(E_INVALIDARG, wsl::shared::Localization::MessageInvalidHexString(std::basic_string<T>{input}));
    }

    std::vector<BYTE> result;
    result.reserve(input.length() / 2);
    T currentHex[3]{};
    for (size_t i = 0; i < input.size(); i += 2)
    {
        // Skip '0x', if any
        if (i == 0 && IsHexSpecifier(input[0], input[1]))
        {
            continue;
        }

        currentHex[0] = input[i];
        currentHex[1] = input[i + 1];
        T* endPtr{};

        const auto byte = ConvertHexByte(currentHex, &endPtr);
        if (endPtr != currentHex + 2)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, wsl::shared::Localization::MessageInvalidHexString(std::basic_string<T>{input}));
        }

        result.push_back(byte);
    }

    return result;
}
} // namespace

std::vector<BYTE> wsl::windows::common::string::HexToBytes(std::string_view input)
{
    return HexToBytesT(input);
}

std::vector<BYTE> wsl::windows::common::string::HexToBytes(std::wstring_view input)
{
    return HexToBytesT(input);
}

std::string wsl::windows::common::string::WideToMultiByte(_In_opt_ LPCWSTR Source, _In_ size_t CharacterCount)
{
    if (CharacterCount == -1)
    {
        CharacterCount = Source ? wcslen(Source) : 0;
    }

    if (CharacterCount == 0)
    {
        return {};
    }

    THROW_HR_IF(E_BOUNDS, (CharacterCount > static_cast<size_t>(std::numeric_limits<int>::max())));

    int required = WideCharToMultiByte(CP_UTF8, 0, Source, gsl::narrow_cast<int>(CharacterCount), nullptr, 0, nullptr, nullptr);
    THROW_LAST_ERROR_IF(required == 0);

    std::string converted(required, '\0');
    required = WideCharToMultiByte(CP_UTF8, 0, Source, gsl::narrow_cast<int>(CharacterCount), converted.data(), required, nullptr, nullptr);
    THROW_LAST_ERROR_IF(required == 0);

    return converted;
}

std::string wsl::windows::common::string::WideToMultiByte(_In_ std::wstring_view Source)
{
    return WideToMultiByte(Source.data(), Source.length());
}

std::optional<uint64_t> wsl::windows::common::string::ParseStorageSize(std::wstring_view String, StorageSizeUnit Unit)
{
    std::wstring_view number;
    std::wstring_view suffix;
    const auto space = String.find(L' ');
    if (space != std::wstring_view::npos)
    {
        number = String.substr(0, space);
        suffix = String.substr(space + 1);
    }
    else
    {
        const auto numberEnd = String.find_last_of(L"0123456789.");
        if (numberEnd == std::wstring_view::npos)
        {
            return {};
        }

        number = String.substr(0, numberEnd + 1);
        suffix = String.substr(numberEnd + 1);
    }

    auto narrowNumber = WideToMultiByte(number);
    if (!narrowNumber.empty() && narrowNumber.front() == '+')
    {
        narrowNumber.erase(0, 1);
    }

    uint64_t multiplier = 1;
    if (!suffix.empty())
    {
        auto normalizedSuffix = wsl::shared::string::AsciiToLower(suffix);
        if (normalizedSuffix != L"b")
        {
            if (normalizedSuffix.size() > 3 || (normalizedSuffix.size() == 2 && normalizedSuffix[1] != L'b') ||
                (normalizedSuffix.size() == 3 && normalizedSuffix.substr(1) != L"ib"))
            {
                return {};
            }

            constexpr std::wstring_view c_memoryUnits = L"kmgtp";
            const auto unitIndex = c_memoryUnits.find(normalizedSuffix[0]);
            if (unitIndex == std::wstring_view::npos)
            {
                return {};
            }

            const uint64_t base = Unit == StorageSizeUnit::Decimal ? 1000 : 1024;
            for (size_t index = 0; index <= unitIndex; ++index)
            {
                multiplier *= base;
            }
        }
    }

    if (!narrowNumber.empty() && narrowNumber.find_first_not_of("0123456789") == std::string::npos)
    {
        uint64_t value{};
        const auto result = std::from_chars(narrowNumber.data(), narrowNumber.data() + narrowNumber.size(), value);
        if (result.ec != std::errc() || result.ptr != narrowNumber.data() + narrowNumber.size() ||
            value > std::numeric_limits<uint64_t>::max() / multiplier)
        {
            return {};
        }

        return value * multiplier;
    }

    // Fractional and exponent forms require floating-point parsing and may lose precision above 2^53.
    double value{};
    const auto result = std::from_chars(narrowNumber.data(), narrowNumber.data() + narrowNumber.size(), value, std::chars_format::general);
    if (result.ec != std::errc() || result.ptr != narrowNumber.data() + narrowNumber.size() || !std::isfinite(value) || value < 0)
    {
        return {};
    }

    const double bytes = value * static_cast<double>(multiplier);
    if (!std::isfinite(bytes) || bytes >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
    {
        return {};
    }

    return static_cast<uint64_t>(bytes);
}

std::wstring wsl::windows::common::string::FormatStorageSize(uint64_t Bytes, StorageSizeUnit Unit, uint32_t DecimalPlaces, bool IncludeSpace)
{
    constexpr size_t c_unitCount = 6;
    constexpr std::array<std::wstring_view, c_unitCount> c_decimalUnits{L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    constexpr std::array<std::wstring_view, c_unitCount> c_binaryUnits{L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB"};

    const double base = Unit == StorageSizeUnit::Decimal ? 1000.0 : 1024.0;
    const auto& units = Unit == StorageSizeUnit::Decimal ? c_decimalUnits : c_binaryUnits;

    double value = static_cast<double>(Bytes);
    size_t unitIndex = 0;
    while (value >= base && unitIndex + 1 < c_unitCount)
    {
        value /= base;
        ++unitIndex;
    }

    const auto formattedValue = unitIndex == 0 ? std::to_wstring(Bytes) : std::format(L"{:.{}f}", value, DecimalPlaces);
    return std::format(L"{}{}{}", formattedValue, IncludeSpace ? L" " : L"", units[unitIndex]);
}

std::wstring wsl::windows::common::string::FormatBytes(uint64_t Bytes)
{
    return FormatStorageSize(Bytes, StorageSizeUnit::Decimal, 2, true);
}

std::wstring wsl::windows::common::string::FormatDockerSize(uint64_t Bytes)
{
    constexpr std::wstring_view c_units[] = {L"B", L"kB", L"MB", L"GB", L"TB", L"PB", L"EB", L"ZB", L"YB"};

    auto value = static_cast<double>(Bytes);
    size_t unitIndex = 0;
    while (value >= 1000.0 && unitIndex + 1 < std::size(c_units))
    {
        value /= 1000.0;
        unitIndex++;
    }

    return std::format(L"{:.3g}{}", value, c_units[unitIndex]);
}

std::wstring wsl::windows::common::string::TruncateId(_In_ std::wstring_view id, bool shortenLength)
{
    return TruncateIdImpl(id, shortenLength);
}

std::string wsl::windows::common::string::TruncateId(_In_ std::string_view id, bool shortenLength)
{
    return TruncateIdImpl(id, shortenLength);
}

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

std::string wsl::windows::common::string::ExpandToRfc3339(const std::string& timestamp)
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

std::int64_t wsl::windows::common::string::Rfc3339ToEpoch(const std::string& timestamp)
{
    // Normalize a trailing 'Z' or 'z' to '+00:00' so that %Ez parses every zone uniformly.
    std::string normalized{timestamp};
    if (!normalized.empty() && (normalized.back() == 'Z' || normalized.back() == 'z'))
    {
        normalized.pop_back();
        normalized += "+00:00";
    }

    // Reject a separator with no fractional digits, which std::chrono::parse otherwise accepts.
    const auto separator = normalized.find('.');
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        separator != std::string::npos &&
            (separator + 1 >= normalized.size() || std::isdigit(static_cast<unsigned char>(normalized[separator + 1])) == 0),
        "Failed to parse timestamp '%hs'",
        timestamp.c_str());

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

    // Parse at nanosecond precision so that fractional seconds are consumed rather than left behind.
    std::chrono::sys_time<std::chrono::nanoseconds> parsed{};
    std::istringstream stream(normalized);
    stream >> std::chrono::parse("%FT%T%Ez", parsed);
    THROW_HR_IF_MSG(E_INVALIDARG, stream.fail(), "Failed to parse timestamp '%hs'", timestamp.c_str());
    THROW_HR_IF_MSG(
        E_INVALIDARG,
        stream.peek() != std::istringstream::traits_type::eof(),
        "Unexpected trailing characters in timestamp '%hs'",
        timestamp.c_str());

    return std::chrono::duration_cast<std::chrono::seconds>(parsed.time_since_epoch()).count();
}

std::optional<std::chrono::nanoseconds> wsl::windows::common::string::TryParseDuration(const std::string& duration)
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

std::string wsl::windows::common::string::EpochToLocalDisplayTime(LONGLONG timestamp)
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

std::string wsl::windows::common::string::Rfc3339ToUtcDisplayTime(std::string_view timestamp)
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
