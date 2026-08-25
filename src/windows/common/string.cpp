/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    string.cpp

Abstract:

    This file contains string helper function definitions.

--*/

#include "precomp.h"
#include <charconv>
#include <cmath>
#include <limits>
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

std::wstring wsl::windows::common::string::FormatHumanReadableSize(uint64_t Bytes, uint32_t Precision, StorageSizeUnit Unit)
{
    constexpr size_t c_unitCount = 9;
    constexpr std::array<std::wstring_view, c_unitCount> c_decimalUnits{
        L"B", L"kB", L"MB", L"GB", L"TB", L"PB", L"EB", L"ZB", L"YB"};
    constexpr std::array<std::wstring_view, c_unitCount> c_binaryUnits{
        L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB", L"EiB", L"ZiB", L"YiB"};

    const double base = Unit == StorageSizeUnit::Decimal ? 1000.0 : 1024.0;
    const auto& units = Unit == StorageSizeUnit::Decimal ? c_decimalUnits : c_binaryUnits;

    auto value = static_cast<double>(Bytes);
    size_t unitIndex = 0;
    while (value >= base && unitIndex + 1 < c_unitCount)
    {
        value /= base;
        unitIndex++;
    }

    return std::format(L"{:.{}g}{}", value, Precision, units[unitIndex]);
}

std::wstring wsl::windows::common::string::TruncateId(_In_ std::wstring_view id, bool shortenLength)
{
    return TruncateIdImpl(id, shortenLength);
}

std::string wsl::windows::common::string::TruncateId(_In_ std::string_view id, bool shortenLength)
{
    return TruncateIdImpl(id, shortenLength);
}

// Returns the number of terminal columns a code point occupies. This mirrors docker's charWidth, which treats
// East Asian wide and fullwidth code points as two columns and everything else as one.
static size_t CharacterWidth(UChar32 CodePoint)
{
    const auto width = u_getIntPropertyValue(CodePoint, UCHAR_EAST_ASIAN_WIDTH);
    return (width == U_EA_WIDE || width == U_EA_FULLWIDTH) ? 2 : 1;
}

std::wstring wsl::windows::common::string::Ellipsis(_In_ std::wstring_view Value, _In_ size_t MaxDisplayWidth)
{
    if (MaxDisplayWidth == 0 || Value.empty())
    {
        return {};
    }

    const auto length = gsl::narrow_cast<int32_t>(Value.size());
    if (MaxDisplayWidth == 1)
    {
        // There is no room for both content and an ellipsis, so the leading code point is kept as-is even
        // if it is wider than the limit.
        int32_t index = 0;
        UChar32 codePoint{};
        U16_NEXT(Value.data(), index, length, codePoint);
        return std::wstring{Value.substr(0, index)};
    }

    // The ellipsis occupies one column, so the retained content has one column less to work with.
    const auto budget = MaxDisplayWidth - 1;
    size_t totalWidth = 0;
    size_t cutoff = 0;
    for (int32_t index = 0; index < length;)
    {
        UChar32 codePoint{};
        U16_NEXT(Value.data(), index, length, codePoint);
        totalWidth += CharacterWidth(codePoint);
        if (totalWidth <= budget)
        {
            cutoff = index;
        }
    }

    // A cutoff of zero means the first code point alone leaves no room for the ellipsis, in which case docker
    // returns the value untouched.
    if (totalWidth <= MaxDisplayWidth || cutoff == 0)
    {
        return std::wstring{Value};
    }

    return std::wstring{Value.substr(0, cutoff)} + L'\u2026';
}
