/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcCredentialStore.cpp

Abstract:

    Implementation of credential store helpers.

--*/

#include "WslcCredentialStore.h"
#include <format>
#include <wincrypt.h>

std::string wsl::windows::common::BuildRegistryAuthHeader(
    const std::string& username, const std::string& password, const std::string& serverAddress)
{
    auto authJson = std::format(
        R"({{"username":"{}","password":"{}","serveraddress":"{}"}})", username, password, serverAddress);

    DWORD base64Size = 0;
    THROW_IF_WIN32_BOOL_FALSE(CryptBinaryToStringA(
        reinterpret_cast<const BYTE*>(authJson.c_str()),
        static_cast<DWORD>(authJson.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
        nullptr,
        &base64Size));

    std::string result(base64Size, '\0');
    THROW_IF_WIN32_BOOL_FALSE(CryptBinaryToStringA(
        reinterpret_cast<const BYTE*>(authJson.c_str()),
        static_cast<DWORD>(authJson.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
        result.data(),
        &base64Size));

    result.resize(base64Size);
    return result;
}
