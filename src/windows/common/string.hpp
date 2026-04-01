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

// Forward declare types to avoid pulling in excessive number of headers.
using IP_ADDRESS_PREFIX = struct _IP_ADDRESS_PREFIX;
using SOCKADDR_INET = union _SOCKADDR_INET;

namespace wsl::windows::common::string {

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
