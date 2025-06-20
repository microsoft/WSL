/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    string.cpp

Abstract:

    This file contains string helper function definitions.

--*/

#include "precomp.h"

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