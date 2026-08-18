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

// Formats a unix timestamp the way docker does, matching Go's time.Time.String() layout. Falls back
// to UTC when the time zone database is unavailable.
std::string FormatDockerTimestamp(LONGLONG timestamp);

// Formats an RFC 3339 timestamp reported by the docker API, e.g. "2026-03-05T10:30:00.123456789Z",
// the way docker formats network timestamps: UTC, with the fractional seconds preserved exactly as
// the daemon reported them. The input is returned unchanged when it cannot be parsed.
std::string FormatDockerTimestamp(std::string_view timestamp);

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
