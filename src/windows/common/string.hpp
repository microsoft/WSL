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

std::string WideToMultiByte(_In_opt_ LPCWSTR Source, _In_ size_t CharacterCount = -1);

std::string WideToMultiByte(_In_ std::wstring_view Source);

struct PhysicalMacAddress
{
    BYTE Address[MAX_ADAPTER_ADDRESS_LENGTH]{};
};

} // namespace wsl::windows::common::string
