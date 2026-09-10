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
inline constexpr const wchar_t DefaultStorageVhdName[] = L"storage.vhdx";
inline constexpr const char DefaultHostLoopback[] = "host.wslc.internal";
inline constexpr uint32_t DefaultBootTimeoutMs = 30000;
inline constexpr const char ContainerdStorageMountPoint[] = "/var/lib/docker";

// Grace period given to a container runtime process to exit after being asked to terminate, and
// the additional time it is given after being killed.
inline constexpr uint32_t ProcessTerminateTimeoutMs = 30 * 1000;
inline constexpr uint32_t ProcessKillTimeoutMs = 10 * 1000;

} // namespace wsl::windows::wslc
