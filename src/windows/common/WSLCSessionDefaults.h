/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WSLCSessionDefaults.h

Abstract:

    Shared constants for WSLc session naming and storage.

--*/
#pragma once

#include <cstdint>

namespace wsl::windows::wslc {

inline constexpr const wchar_t DefaultSessionName[] = L"wslc-cli";
inline constexpr const wchar_t DefaultAdminSessionName[] = L"wslc-cli-admin";
inline constexpr const wchar_t DefaultStorageSubPath[] = L"wslc\\sessions";
inline constexpr uint32_t DefaultBootTimeoutMs = 30000;

} // namespace wsl::windows::wslc
