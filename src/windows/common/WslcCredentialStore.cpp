/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcCredentialStore.cpp

Abstract:

    Implementation of credential store helpers.

--*/

#include "precomp.h"
#include "WslcCredentialStore.h"
#include <wincrypt.h>

namespace {

std::string Base64Encode(const std::string& input)
{
    DWORD base64Size = 0;
    THROW_IF_WIN32_BOOL_FALSE(CryptBinaryToStringA(
        reinterpret_cast<const BYTE*>(input.c_str()), static_cast<DWORD>(input.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &base64Size));

    auto buffer = std::make_unique<char[]>(base64Size);
    THROW_IF_WIN32_BOOL_FALSE(CryptBinaryToStringA(
        reinterpret_cast<const BYTE*>(input.c_str()),
        static_cast<DWORD>(input.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
        buffer.get(),
        &base64Size));

    return std::string(buffer.get());
}

} // namespace

std::string wsl::windows::common::BuildRegistryAuthHeader(const std::string& username, const std::string& password, const std::string& serverAddress)
{
    nlohmann::json authJson = {{"username", username}, {"password", password}, {"serveraddress", serverAddress}};

    return Base64Encode(authJson.dump());
}

std::string wsl::windows::common::BuildRegistryAuthHeader(const std::string& identityToken, const std::string& serverAddress)
{
    nlohmann::json authJson = {{"identitytoken", identityToken}, {"serveraddress", serverAddress}};

    return Base64Encode(authJson.dump());
}
