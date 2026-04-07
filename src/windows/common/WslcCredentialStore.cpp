/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcCredentialStore.cpp

Abstract:

    Implementation of credential store helpers.

--*/

#include "precomp.h"
#include "WslcCredentialStore.h"
#include "wslutil.h"

std::string wsl::windows::common::BuildRegistryAuthHeader(const std::string& username, const std::string& password, const std::string& serverAddress)
{
    nlohmann::json authJson = {{"username", username}, {"password", password}, {"serveraddress", serverAddress}};

    return wslutil::Base64Encode(authJson.dump());
}

std::string wsl::windows::common::BuildRegistryAuthHeader(const std::string& identityToken, const std::string& serverAddress)
{
    nlohmann::json authJson = {{"identitytoken", identityToken}, {"serveraddress", serverAddress}};

    return wslutil::Base64Encode(authJson.dump());
}
