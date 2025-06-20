/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    defs.h

Abstract:

    This file contains shared platform definitions.

--*/

#pragma once

#if defined(_MSC_VER)
#define THROW_INVALID_ARG_IF(condition) THROW_HR_IF(E_INVALIDARG, condition)
#elif defined(__GNUC__)
#define THROW_INVALID_ARG_IF(condition) THROW_ERRNO_IF(EINVAL, condition)
#define _stricmp strcasecmp
#define _wcsicmp wcscasecmp
#endif

namespace wsl::shared {

inline constexpr std::uint32_t VersionMajor = WSL_PACKAGE_VERSION_MAJOR;
inline constexpr std::uint32_t VersionMinor = WSL_PACKAGE_VERSION_MINOR;
inline constexpr std::uint32_t VersionRevision = WSL_PACKAGE_VERSION_REVISION;
inline constexpr std::tuple<uint32_t, uint32_t, uint32_t> PackageVersion{VersionMajor, VersionMinor, VersionRevision};

#ifdef WSL_OFFICIAL_BUILD

inline constexpr bool OfficialBuild = true;

#else

inline constexpr bool OfficialBuild = false;

#endif

#ifdef DEBUG

inline constexpr bool Debug = true;

#else

inline constexpr bool Debug = false;

#endif

#ifdef _AMD64_

inline constexpr bool Arm64 = false;

#elif _ARM64_

inline constexpr bool Arm64 = true;

#else

#error Unsupported compiler or build environment

#endif

} // namespace wsl::shared
