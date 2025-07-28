/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslInstall.cpp

Abstract:

    This file contains implementations for installing WSL distributions

--*/

#include "precomp.h"
#include "WslInstall.h"
#include "registry.hpp"
#include "wslutil.h"
#include "Distribution.h"
#include "HandleConsoleProgressBar.h"
#include "svccomm.hpp"

extern HINSTANCE g_dllInstance;

constexpr LPCWSTR c_optionalFeatureInstallStatus = L"InstallStatus";
constexpr LPCWSTR c_optionalFeatureNameVmp = L"VirtualMachinePlatform";
constexpr LPCWSTR c_optionalFeatureNameWsl = L"Microsoft-Windows-Subsystem-Linux";

using wsl::shared::Localization;
using namespace wsl::windows::common::distribution;
using namespace wsl::windows::common::wslutil;

namespace {
std::vector<BYTE> ParseHex(const std::wstring& input);

void EnforceFileHash(HANDLE file, const std::wstring& expectedHash)
{
    wsl::windows::common::ExecutionContext context(wsl::windows::common::VerifyChecksum);

    const auto fileHash = wsl::windows::common::wslutil::HashFile(file, CALG_SHA_256);

    THROW_LAST_ERROR_IF(SetFilePointer(file, 0, 0, FILE_BEGIN) == INVALID_SET_FILE_POINTER);
    if (fileHash != ParseHex(expectedHash))
    {
        THROW_HR_WITH_USER_ERROR(
            TRUST_E_BAD_DIGEST,
            wsl::shared::Localization::MessageHashMismatch(
                expectedHash.c_str(), wsl::windows::common::string::BytesToHex(fileHash).c_str()));
    }
}

std::vector<std::wstring> GetInstalledOptionalComponents()
{
    // Query the list of optional components that have already been installed.
    const auto lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
    auto [key, error] = wsl::windows::common::registry::OpenKeyNoThrow(lxssKey.get(), c_optionalFeatureInstallStatus, KEY_READ);
    std::vector<std::wstring> installedComponents;
    if (key)
    {
        const auto components = wsl::windows::common::registry::ReadString(key.get(), nullptr, nullptr, L"");
        installedComponents = wsl::shared::string::Split(components, L',');
    }

    return installedComponents;
}

std::vector<BYTE> ParseHex(const std::wstring& input)
{
    std::vector<BYTE> result;
    for (auto i = 0; i < input.size(); i += 2)
    {
        // Skip '0x', if any
        if (i == 0 && input[0] == '0' && tolower(input[1]) == 'x')
        {
            continue;
        }

        auto current = input.substr(i, 2);
        wchar_t* endPtr{};

        const auto byte = wcstoul(current.data(), &endPtr, 16);
        if (endPtr != current.data() + 2)
        {
            THROW_HR_WITH_USER_ERROR(E_INVALIDARG, wsl::shared::Localization::MessageInvalidHexString(input.c_str()));
        }

        result.push_back(static_cast<BYTE>(byte));
    }

    return result;
}
}; // namespace

HRESULT WslInstall::InstallDistribution(
    _Out_ InstallResult& installResult,
    _In_ const std::optional<std::wstring>& distributionName,
    _In_ const std::optional<ULONG>& version,
    _In_ bool launchAfterInstall,
    _In_ bool useGitHub,
    _In_ bool legacy,
    _In_ bool fixedVhd,
    _In_ const std::optional<std::wstring>& localName,
    _In_ const std::optional<std::wstring>& location,
    _In_ const std::optional<uint64_t>& vhdSize)
try
{
    wsl::windows::common::ExecutionContext context(wsl::windows::common::InstallDistro);

    try
    {
        const auto distributions = wsl::windows::common::distribution::GetAvailable();

        if (distributionName.has_value())
        {
            installResult.Distribution = LookupByName(distributions, distributionName->c_str(), legacy);
        }
        else
        {
            if (legacy)
            {
                THROW_HR_IF(
                    E_UNEXPECTED, !distributions.Manifest.Distributions.has_value() || distributions.Manifest.Distributions->empty());
                installResult.Distribution = (*distributions.Manifest.Distributions)[0];
            }
            else
            {
                if (distributions.OverrideManifest.has_value() && distributions.OverrideManifest->Default.has_value())
                {
                    installResult.Distribution = LookupByName(distributions, distributions.OverrideManifest->Default->c_str(), false);
                }
                else
                {
                    if (!distributions.Manifest.Default.has_value())
                    {
                        THROW_HR_WITH_USER_ERROR(E_UNEXPECTED, wsl::shared::Localization::MessageNoInstallDefault());
                    }

                    installResult.Distribution = LookupByName(distributions, distributions.Manifest.Default->c_str(), false);
                }
            }
        }

        if (const auto* distro = std::get_if<ModernDistributionVersion>(&*installResult.Distribution))
        {
            std::tie(installResult.Name, installResult.Id) =
                InstallModernDistribution(*distro, version, localName, location, vhdSize, fixedVhd);

            installResult.InstalledViaGitHub = true;
        }
        else if (const auto* distro = std::get_if<Distribution>(&*installResult.Distribution))
        {
            std::list<std::pair<bool, LPCWSTR>> unsupportedArguments = {
                {localName.has_value(), WSL_INSTALL_ARG_NAME_LONG},
                {location.has_value(), WSL_INSTALL_ARG_LOCATION_LONG},
                {vhdSize.has_value(), WSL_INSTALL_ARG_VHD_SIZE},
                {fixedVhd, WSL_INSTALL_ARG_FIXED_VHD}};

            for (const auto& [condition, argument] : unsupportedArguments)
            {
                if (condition)
                {
                    THROW_HR_WITH_USER_ERROR(WSL_E_INVALID_USAGE, Localization::MessageNotSupportedOnLegacyDistros(argument).c_str());
                }
            }

            installResult.Alreadyinstalled = wsl::windows::common::distribution::IsInstalled(*distro, useGitHub);
            if (!installResult.Alreadyinstalled)
            {
                EMIT_USER_WARNING(Localization::MessageUsingLegacyDistribution());
                if (version.has_value())
                {
                    THROW_HR_WITH_USER_ERROR(WSL_E_INVALID_USAGE, Localization::MessageLegacyDistributionVersionArgNotSupported());
                }

                // If downloading from the store fails, attempt to download from GitHub.
                if (!useGitHub)
                {
                    auto hr =
                        wil::ResultFromException([&]() { wsl::windows::common::distribution::LegacyInstallViaStore(*distro); });
                    if (FAILED(hr))
                    {
                        useGitHub = true;
                        auto errorString = wsl::windows::common::wslutil::GetErrorString(hr);
                        wsl::windows::common::wslutil::PrintMessage(
                            Localization::MessageDistroStoreInstallFailed(distro->Name.c_str(), errorString.c_str()), stdout);
                    }
                }

                if (useGitHub)
                {
                    wsl::windows::common::distribution::LegacyInstallViaGithub(*distro);
                }
            }

            installResult.Name = distro->FriendlyName;
            installResult.InstalledViaGitHub = useGitHub;
        }
        else
        {
            THROW_HR(E_UNEXPECTED);
        }
    }
    catch (...)
    {
        // Rethrowing via WIL is required for the error context to be properly set
        // in case a winrt exception was thrown.
        THROW_HR(wil::ResultFromCaughtException());
    }

    return S_OK;
}
CATCH_RETURN()

std::pair<bool, std::vector<std::wstring>> WslInstall::CheckForMissingOptionalComponents(_In_ bool requireWslOptionalComponent)
{
    // Include the WSL optional component if it was requested, or if the OS is not Windows 11 or later.
    std::vector<std::wstring> missingComponents;
    requireWslOptionalComponent |= !wsl::windows::common::helpers::IsWindows11OrAbove();
    if (requireWslOptionalComponent && !wsl::windows::common::helpers::IsServicePresent(L"lxssmanager"))
    {
        missingComponents.emplace_back(c_optionalFeatureNameWsl);
    }

    if (!wsl::windows::common::wslutil::IsVirtualMachinePlatformInstalled())
    {
        missingComponents.emplace_back(c_optionalFeatureNameVmp);
    }

    // If any required components are not present, a reboot is required.
    bool rebootRequired = !missingComponents.empty();

    // Query the list of optional components that have already been installed.
    const auto installedComponents = GetInstalledOptionalComponents();
    for (const auto& component : installedComponents)
    {
        std::erase(missingComponents, component);
    }

    return {rebootRequired, std::move(missingComponents)};
}

void WslInstall::InstallOptionalComponents(const std::vector<std::wstring>& components)
{
    std::wstring systemDirectory;
    THROW_IF_FAILED(wil::GetSystemDirectoryW(systemDirectory));

    const auto dismPath = std::filesystem::path(std::move(systemDirectory)) / L"dism.exe";
    for (const auto& component : components)
    {
        wsl::windows::common::wslutil::PrintMessage(Localization::MessageInstallingWindowsComponent(component));

        auto commandLine = std::format(L"{} /Online /NoRestart /enable-feature /featurename:{}", dismPath.wstring(), component);
        const auto exitCode = wsl::windows::common::helpers::RunProcess(commandLine);
        if (exitCode != 0 && exitCode != ERROR_SUCCESS_REBOOT_REQUIRED)
        {
            THROW_HR_WITH_USER_ERROR(WSL_E_INSTALL_COMPONENT_FAILED, Localization::MessageOptionalComponentInstallFailed(component, exitCode));
        }
    }

    // Update the list of optional components that have been installed.
    auto installedComponents = GetInstalledOptionalComponents();
    installedComponents.insert(installedComponents.end(), components.begin(), components.end());
    const auto lxssKey = wsl::windows::common::registry::OpenLxssUserKey();
    const auto key =
        wsl::windows::common::registry::CreateKey(lxssKey.get(), c_optionalFeatureInstallStatus, KEY_ALL_ACCESS, nullptr, REG_OPTION_VOLATILE);
    wsl::windows::common::registry::WriteString(key.get(), nullptr, nullptr, wsl::shared::string::Join(installedComponents, L',').c_str());
}

std::pair<std::wstring, GUID> WslInstall::InstallModernDistribution(
    const ModernDistributionVersion& distribution,
    const std::optional<ULONG>& version,
    const std::optional<std::wstring>& name,
    const std::optional<std::wstring>& location,
    const std::optional<uint64_t>& vhdSize,
    const bool fixedVhd)
{
    wsl::windows::common::SvcComm service;

    // Fail early if the distributions name is already in use.
    auto result = wil::ResultFromException([&]() {
        service.GetDistributionId(name.has_value() ? name->c_str() : distribution.Name.c_str(), LXSS_GET_DISTRO_ID_LIST_ALL);
    });

    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), SUCCEEDED(result));
    LOG_HR_IF(result, result != WSL_E_DISTRO_NOT_FOUND);

    const auto downloadInfo = wsl::shared::Arm64 ? distribution.Arm64Url : distribution.Amd64Url;
    THROW_HR_IF(E_UNEXPECTED, !downloadInfo.has_value());

    std::wstring installPath;
    bool fileDownloaded{};
    auto deleteFile = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
        if (fileDownloaded)
        {
            THROW_IF_WIN32_BOOL_FALSE(DeleteFileW(installPath.c_str()));
        }
    });

    if (auto localFile = wsl::windows::common::filesystem::TryGetPathFromFileUrl(downloadInfo->Url))
    {
        installPath = std::move(localFile.value());
    }
    else
    {
        PrintMessage(Localization::MessageDownloading(distribution.FriendlyName.c_str()), stdout);
        installPath = DownloadFile(downloadInfo->Url, distribution.Name + L".wsl");
        fileDownloaded = true;
    }

    PrintMessage(Localization::MessageInstalling(distribution.FriendlyName.c_str()), stdout);

    wil::unique_handle file{CreateFile(installPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
    THROW_LAST_ERROR_IF(!file);

    EnforceFileHash(file.get(), downloadInfo->Sha256);

    wsl::windows::common::HandleConsoleProgressBar progressBar(file.get(), Localization::MessageImportProgress());

    auto [id, installedName] = service.RegisterDistribution(
        name.has_value() ? name->c_str() : distribution.Name.c_str(),
        version.value_or(LXSS_WSL_VERSION_DEFAULT),
        file.get(),
        location.has_value() ? location->c_str() : nullptr,
        fixedVhd ? LXSS_IMPORT_DISTRO_FLAGS_FIXED_VHD : 0,
        vhdSize);

    return {installedName.get(), id};
}