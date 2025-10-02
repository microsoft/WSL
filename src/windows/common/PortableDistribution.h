/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PortableDistribution.h

Abstract:

    This file contains definitions and functions for portable WSL distributions
    that can run from removable media (USB drives, external disks, etc.).

--*/

#pragma once

#include <optional>
#include <string>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "JsonUtils.h"

namespace wsl::windows::common::portable {

// Metadata structure for portable distributions stored in wsl-portable.json
struct PortableDistributionMetadata
{
    std::wstring Name;                          // Distribution name
    std::wstring FriendlyName;                  // Human-readable name
    std::optional<std::wstring> VhdxPath;       // Relative or absolute path to VHDX file
    ULONG Version;                              // WSL version (1 or 2)
    ULONG DefaultUid;                           // Default user ID
    std::optional<GUID> Guid;                   // Distribution GUID (generated on first mount)
    std::optional<std::wstring> DefaultUser;    // Default username
    bool IsPortable;                            // Flag indicating this is a portable distribution
    
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
        PortableDistributionMetadata,
        Name,
        FriendlyName,
        VhdxPath,
        Version,
        DefaultUid,
        Guid,
        DefaultUser,
        IsPortable);
};

// Structure to hold portable mount results
struct PortableMountResult
{
    GUID DistroGuid;
    std::wstring DistroName;
    std::filesystem::path VhdxPath;
    bool NewlyCreated;
};

// Check if a path is on removable media
// Set allowFixed to true to permit fixed drives for development/testing
bool IsRemovableDrive(_In_ const std::filesystem::path& path, _In_ bool allowFixed = false);

// Check if a path is using Volume GUID format (\\?\Volume{UUID}\...)
bool IsVolumeGuidPath(_In_ const std::wstring& path);

// Normalize a path for portable storage (convert to relative if possible)
std::filesystem::path NormalizePortablePath(_In_ const std::filesystem::path& basePath, _In_ const std::filesystem::path& targetPath);

// Read portable distribution metadata from wsl-portable.json
PortableDistributionMetadata ReadPortableMetadata(_In_ const std::filesystem::path& metadataPath);

// Write portable distribution metadata to wsl-portable.json
void WritePortableMetadata(_In_ const std::filesystem::path& metadataPath, _In_ const PortableDistributionMetadata& metadata);

// Mount a portable distribution from removable media
PortableMountResult MountPortableDistribution(
    _In_ const std::filesystem::path& portablePath,
    _In_opt_ LPCWSTR distroName = nullptr,
    _In_ bool temporary = false,
    _In_ bool allowFixed = false);

// Unmount and cleanup a portable distribution
void UnmountPortableDistribution(_In_ const GUID& distroGuid, _In_ bool removeRegistration = true);

// Check if a distribution is registered as portable
bool IsPortableDistribution(_In_ const GUID& distroGuid);

// Get the portable base path for a portable distribution
std::optional<std::filesystem::path> GetPortableBasePath(_In_ const GUID& distroGuid);

// Validate that a path is suitable for portable WSL usage
void ValidatePortablePath(_In_ const std::filesystem::path& path, _In_ bool allowFixed = false);

// Create a new portable distribution from a tar/vhdx file
void CreatePortableDistribution(
    _In_ const std::filesystem::path& portablePath,
    _In_ LPCWSTR distroName,
    _In_ const std::filesystem::path& sourceFile,
    _In_ ULONG version,
    _In_ ULONG flags,
    _In_ bool allowFixed = false);

} // namespace wsl::windows::common::portable
