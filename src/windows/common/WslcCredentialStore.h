/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslcCredentialStore.h

Abstract:

    Helpers for building Docker/OCI registry credential payloads.

--*/

#pragma once
#include <string>

namespace wsl::windows::common {

// Builds the base64-encoded X-Registry-Auth header value used by Docker APIs
// (PullImage, PushImage, etc.) from the given credentials.
std::string BuildRegistryAuthHeader(const std::string& username, const std::string& password, const std::string& serverAddress);

} // namespace wsl::windows::common
