/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PortableDistribution.cpp

Abstract:

    This file contains the implementation of portable WSL distribution functionality.

--*/

#include "precomp.h"
#include "PortableDistribution.h"
#include "svccomm.hpp"
#include "registry.hpp"
#include "filesystem.hpp"
#include "string.hpp"
#include "helpers.hpp"
#include <fstream>
#include <winioctl.h>

namespace wsl::windows::common::portable {

constexpr inline auto c_portableMetadataFileName = L"wsl-portable.json";
constexpr inline auto c_portableRegistryValue = L"PortableBasePath";
constexpr inline auto c_portableFlagValue = L"IsPortable";
constexpr inline auto c_portableTemporaryValue = L"IsTemporary";

bool IsRemovableDrive(_In_ const std::filesystem::path& path, _In_ bool allowFixed)
{
    // Get the root path of the drive
    auto rootPath = path.root_path();
    if (rootPath.empty())
    {
        // If no root, try to get the absolute path
        std::error_code ec;
        auto absPath = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return false;
        }
        rootPath = absPath.root_path();
    }

    // For Volume GUID paths like \\?\Volume{UUID}\, extract the volume
    auto pathStr = rootPath.wstring();
    if (IsVolumeGuidPath(pathStr))
    {
        // Extract volume GUID and check its properties
        // Volume GUID format: \\?\Volume{GUID}\
        auto volumeStart = pathStr.find(L"Volume{");
        if (volumeStart != std::wstring::npos)
        {
            auto volumeEnd = pathStr.find(L"}", volumeStart);
            if (volumeEnd != std::wstring::npos)
            {
                // Extract the volume path (including the trailing backslash)
                volumeEnd = pathStr.find(L"\\", volumeEnd);
                if (volumeEnd != std::wstring::npos)
                {
                    pathStr = pathStr.substr(0, volumeEnd + 1);
                }
            }
        }
    }

    const UINT driveType = GetDriveTypeW(pathStr.c_str());
    
    // Restrict to removable media by default for security
    // Allow fixed drives only when explicitly requested (for development/testing)
    if (driveType == DRIVE_REMOVABLE)
    {
        return true;
    }
    
    return allowFixed && (driveType == DRIVE_FIXED);
}

bool IsVolumeGuidPath(_In_ const std::wstring& path)
{
    // Check if path starts with \\?\Volume{ format
    return path.find(L"\\\\?\\Volume{") == 0 || path.find(L"\\??\\Volume{") == 0;
}

std::filesystem::path NormalizePortablePath(
    _In_ const std::filesystem::path& basePath,
    _In_ const std::filesystem::path& targetPath)
{
    std::error_code ec;
    auto relativePath = std::filesystem::relative(targetPath, basePath, ec);
    
    if (!ec && !relativePath.empty())
    {
        return relativePath;
    }
    
    // If we can't make it relative, return absolute path
    return std::filesystem::absolute(targetPath, ec);
}

PortableDistributionMetadata ReadPortableMetadata(_In_ const std::filesystem::path& metadataPath)
{
    std::ifstream file(metadataPath);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), !file.is_open());

    nlohmann::json jsonData;
    try
    {
        file >> jsonData;
    }
    catch (const nlohmann::json::exception& e)
    {
        THROW_HR_MSG(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Failed to parse portable metadata: %s", e.what());
    }

    PortableDistributionMetadata metadata;
    try
    {
        metadata = jsonData.get<PortableDistributionMetadata>();
    }
    catch (const nlohmann::json::exception& e)
    {
        THROW_HR_MSG(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), "Invalid portable metadata format: %s", e.what());
    }

    return metadata;
}

void WritePortableMetadata(_In_ const std::filesystem::path& metadataPath, _In_ const PortableDistributionMetadata& metadata)
{
    nlohmann::json jsonData = metadata;
    
    std::ofstream file(metadataPath);
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_CANNOT_MAKE), !file.is_open());

    try
    {
        file << jsonData.dump(4); // Pretty print with 4-space indentation
    }
    catch (const std::exception& e)
    {
        THROW_HR_MSG(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT), "Failed to write portable metadata: %s", e.what());
    }
}

PortableMountResult MountPortableDistribution(
    _In_ const std::filesystem::path& portablePath,
    _In_opt_ LPCWSTR distroName,
    _In_ bool temporary,
    _In_ bool allowFixed)
{
    // Validate the portable path
    ValidatePortablePath(portablePath, allowFixed);

    // Look for metadata file
    auto metadataPath = portablePath / c_portableMetadataFileName;
    
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
        !std::filesystem::exists(metadataPath),
        "Portable metadata file not found. Expected: %ls",
        metadataPath.c_str());

    // Read metadata
    auto metadata = ReadPortableMetadata(metadataPath);

    // Use provided name or fall back to metadata name
    std::wstring actualName = distroName ? distroName : metadata.Name;

    // Find the VHDX file
    std::filesystem::path vhdxPath;
    if (metadata.VhdxPath.has_value())
    {
        vhdxPath = portablePath / metadata.VhdxPath.value();
    }
    else
    {
        // Try to find a VHDX file in the directory
        for (const auto& entry : std::filesystem::directory_iterator(portablePath))
        {
            if (entry.is_regular_file() && entry.path().extension() == L".vhdx")
            {
                vhdxPath = entry.path();
                break;
            }
        }
    }

    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
        vhdxPath.empty() || !std::filesystem::exists(vhdxPath),
        "VHDX file not found in portable directory");

    // Register the distribution
    wsl::windows::common::SvcComm service;
    ULONG flags = LXSS_IMPORT_DISTRO_FLAGS_VHD | LXSS_IMPORT_DISTRO_FLAGS_NO_OOBE;

    // Import the distribution in-place
    GUID distroGuid;
    try
    {
        distroGuid = service.ImportDistributionInplace(actualName.c_str(), vhdxPath.c_str());
    }
    catch (...)
    {
        // If import fails, try to provide helpful error message
        THROW_HR_MSG(
            wil::ResultFromCaughtException(),
            "Failed to mount portable distribution from %ls",
            vhdxPath.c_str());
    }

    // Store portable metadata in registry
    try
    {
        const wil::unique_hkey lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
        const auto guidString = wsl::shared::string::GuidToString<wchar_t>(distroGuid);
        const wil::unique_hkey distroKey = wsl::windows::common::registry::OpenKey(lxssKey.get(), guidString.c_str(), false);
        
        // Mark as portable
        wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, c_portableFlagValue, 1);
        
        // Store base path for cleanup
        wsl::windows::common::registry::WriteString(
            distroKey.get(),
            nullptr,
            c_portableRegistryValue,
            portablePath.wstring().c_str());
        
        // Track temporary flag for potential auto-cleanup on reboot/logout
        if (temporary)
        {
            wsl::windows::common::registry::WriteDword(distroKey.get(), nullptr, c_portableTemporaryValue, 1);
        }
    }
    catch (...)
    {
        // If we can't write registry, unregister and fail
        try
        {
            service.UnregisterDistribution(&distroGuid);
        }
        catch (...) {}
        
        throw;
    }

    PortableMountResult result;
    result.DistroGuid = distroGuid;
    result.DistroName = actualName;
    result.VhdxPath = vhdxPath;
    result.NewlyCreated = false;

    return result;
}

void UnmountPortableDistribution(_In_ const GUID& distroGuid, _In_ bool removeRegistration)
{
    if (!IsPortableDistribution(distroGuid))
    {
        return;
    }

    wsl::windows::common::SvcComm service;
    
    // Terminate any running instances
    try
    {
        service.TerminateInstance(&distroGuid);
    }
    catch (...) {}

    // Unregister the distribution
    if (removeRegistration)
    {
        try
        {
            service.UnregisterDistribution(&distroGuid);
        }
        catch (...) {}

        // Clean up registry entries
        try
        {
            const wil::unique_hkey lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
            const auto guidString = wsl::shared::string::GuidToString<wchar_t>(distroGuid);
            RegDeleteKeyW(lxssKey.get(), guidString.c_str());
        }
        catch (...) {}
    }
}

bool IsPortableDistribution(_In_ const GUID& distroGuid)
{
    try
    {
        const wil::unique_hkey lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
        const auto guidString = wsl::shared::string::GuidToString<wchar_t>(distroGuid);
        const wil::unique_hkey distroKey = wsl::windows::common::registry::OpenKey(lxssKey.get(), guidString.c_str(), true);
        
        if (!distroKey)
        {
            return false;
        }

        DWORD isPortable = 0;
        if (SUCCEEDED(wsl::windows::common::registry::ReadDword(distroKey.get(), nullptr, c_portableFlagValue, isPortable)))
        {
            return isPortable != 0;
        }
    }
    catch (...) {}

    return false;
}

std::optional<std::filesystem::path> GetPortableBasePath(_In_ const GUID& distroGuid)
{
    try
    {
        const wil::unique_hkey lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
        const auto guidString = wsl::shared::string::GuidToString<wchar_t>(distroGuid);
        const wil::unique_hkey distroKey = wsl::windows::common::registry::OpenKey(lxssKey.get(), guidString.c_str(), true);
        
        if (!distroKey)
        {
            return std::nullopt;
        }

        wil::unique_cotaskmem_string pathStr;
        if (SUCCEEDED(wsl::windows::common::registry::ReadString(distroKey.get(), nullptr, c_portableRegistryValue, pathStr)))
        {
            return std::filesystem::path(pathStr.get());
        }
    }
    catch (...) {}

    return std::nullopt;
}

void ValidatePortablePath(_In_ const std::filesystem::path& path, _In_ bool allowFixed)
{
    // Check if path exists
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND),
        !std::filesystem::exists(path),
        "Portable path does not exist: %ls",
        path.c_str());

    // Check if it's a directory
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_DIRECTORY),
        !std::filesystem::is_directory(path),
        "Portable path must be a directory: %ls",
        path.c_str());

    // Check if it's on removable media (restrict to removable by default)
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
        !IsRemovableDrive(path, allowFixed),
        "Portable path must be on removable media: %ls. To allow fixed drives, use the --allow-fixed flag.",
        path.c_str());
}

void CreatePortableDistribution(
    _In_ const std::filesystem::path& portablePath,
    _In_ LPCWSTR distroName,
    _In_ const std::filesystem::path& sourceFile,
    _In_ ULONG version,
    _In_ ULONG flags,
    _In_ bool allowFixed)
{
    // Validate portable path
    ValidatePortablePath(portablePath, allowFixed);

    // Check if metadata already exists
    auto metadataPath = portablePath / c_portableMetadataFileName;
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_FILE_EXISTS),
        std::filesystem::exists(metadataPath),
        "Portable distribution already exists at: %ls",
        portablePath.c_str());

    // Determine target VHDX path
    std::wstring vhdxFileName = distroName;
    vhdxFileName += L".vhdx";
    auto vhdxPath = portablePath / vhdxFileName;

    // Import the distribution using WSL service
    // We'll register it temporarily to create the VHDX, then manually clean up the registry
    wsl::windows::common::SvcComm service;
    
    wil::unique_hfile sourceFileHandle;
    sourceFileHandle.reset(CreateFileW(
        sourceFile.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    
    THROW_LAST_ERROR_IF(!sourceFileHandle);

    // Create the VHDX in the portable location
    // The RegisterDistribution service handles tar extraction and VHDX creation
    auto [guid, name] = service.RegisterDistribution(
        distroName,
        version,
        sourceFileHandle.get(),
        portablePath.c_str(),
        flags | LXSS_IMPORT_DISTRO_FLAGS_VHD);

    // Clean up the registry entry without deleting the VHDX file
    // We do this manually to avoid the full UnregisterDistribution which would delete the VHDX
    try
    {
        // Open the LXSS registry key
        auto lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
        
        // Convert GUID to string for registry key name
        auto guidStr = wsl::windows::common::string::GuidToString<wchar_t>(guid);
        
        // Delete only the distribution's registry key, leaving the VHDX intact
        wsl::windows::common::registry::DeleteKey(lxssKey.get(), guidStr.c_str());
        
        // Note: We intentionally do NOT call UnregisterDistribution here because
        // it would delete the VHDX file we just created. Instead, we manually remove
        // just the registry entry, leaving the VHDX for portable use.
    }
    catch (...)
    {
        // If cleanup fails, log but continue - the VHDX was created successfully
        // The orphaned registry entry will be cleaned up if the user tries to use
        // the distribution and it doesn't exist
        LOG_CAUGHT_EXCEPTION();
    }

    // Create portable metadata
    PortableDistributionMetadata metadata;
    metadata.Name = distroName;
    metadata.FriendlyName = distroName;
    metadata.VhdxPath = vhdxFileName;
    metadata.Version = version;
    metadata.DefaultUid = 1000; // Standard default
    metadata.IsPortable = true;

    // Write metadata
    WritePortableMetadata(metadataPath, metadata);
}

} // namespace wsl::windows::common::portable
