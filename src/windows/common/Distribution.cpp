/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Distribution.cpp

Abstract:

    This file contains implementations for distribution app download, install and launch.

--*/

#include "precomp.h"
#include "Distribution.h"
#include "ConsoleProgressBar.h"
#include "registry.hpp"

constexpr auto c_defaultDistroListUrl =
    L"https://raw.githubusercontent.com/microsoft/WSL/master/distributions/DistributionInfo.json";
constexpr auto StoreClientId = L"wsl-install-lifted";

using namespace winrt::Windows::ApplicationModel::Store::Preview::InstallControl;
using namespace winrt::Windows::System;
using namespace wsl::windows::common::distribution;
using wsl::shared::Localization;

namespace {
std::wstring GetFamilyNameFromStorePackage(const winrt::Windows::Services::Store::StoreProduct& Package)
{
    const auto extendedJson = Package.ExtendedJsonData();

    const auto json = nlohmann::json::parse(wsl::shared::string::WideToMultiByte(extendedJson.c_str()));
    THROW_HR_IF_MSG(E_UNEXPECTED, !json.contains("Properties"), "Failed to deserialize store json : '%ls'", extendedJson.c_str());

    const auto properties = json.at("Properties");
    THROW_HR_IF_MSG(
        E_UNEXPECTED,
        !properties.is_object() || !properties.contains("PackageFamilyName"),
        "Failed to deserialize store json : '%ls'",
        extendedJson.c_str());

    return properties.at("PackageFamilyName").get<std::wstring>();
}

winrt::Windows::Services::Store::StoreProduct GetStorePackage(LPCWSTR AppId)
{
    const auto storeContext = winrt::Windows::Services::Store::StoreContext::GetDefault();

    const auto productKinds = winrt::single_threaded_vector<winrt::hstring>({L"Application"});
    const auto productIds = winrt::single_threaded_vector<winrt::hstring>({AppId});
    const auto packages = storeContext.GetStoreProductsAsync(productKinds, productIds).get().Products();
    THROW_HR_IF_MSG(
        E_UNEXPECTED,
        packages.Size() != 1,
        "Unexpected store package count AppId=%ls, Count=%zu",
        AppId,
        static_cast<size_t>(packages.Size()));

    return packages.First().Current().Value();
}

std::optional<winrt::Windows::ApplicationModel::Package> GetInstalledPackage(LPCWSTR PackageFamilyName)
{
    const winrt::Windows::Management::Deployment::PackageManager packageManager;
    const auto familyCollection = packageManager.FindPackagesForUser(L"", PackageFamilyName);
    const auto iter = familyCollection.First();
    if (!iter.HasCurrent())
    {
        return {};
    }

    auto package = iter.Current();
    LOG_HR_IF_MSG(E_UNEXPECTED, iter.MoveNext(), "More than one package found for packageFamily=%ls", PackageFamilyName);

    return package;
}

std::wstring GetFamilyName(const Distribution& distro, bool directDownload)
{
    if (directDownload)
    {
        THROW_HR_IF(E_UNEXPECTED, !distro.PackageFamilyName.has_value());
        return *distro.PackageFamilyName;
    }

    return GetFamilyNameFromStorePackage(GetStorePackage(distro.StoreAppId.c_str()));
}

DistributionList ReadFromManifest(const std::wstring& url)
{
    using namespace wsl::windows::common::distribution;

    try
    {
        std::wstring content;
        if (const auto localFile = wsl::windows::common::filesystem::TryGetPathFromFileUrl(url))
        {
            content = wsl::shared::string::ReadFile<wchar_t, wchar_t>(localFile->c_str());
        }
        else
        {
            const winrt::Windows::Web::Http::Filters::HttpBaseProtocolFilter filter;
            filter.CacheControl().WriteBehavior(winrt::Windows::Web::Http::Filters::HttpCacheWriteBehavior::NoCache);
            filter.CacheControl().ReadBehavior(winrt::Windows::Web::Http::Filters::HttpCacheReadBehavior::NoCache);

            const winrt::Windows::Web::Http::HttpClient client(filter);
            const auto response = client.GetAsync(winrt::Windows::Foundation::Uri(url)).get();
            response.EnsureSuccessStatusCode();

            content = response.Content().ReadAsStringAsync().get();
        }

        auto distros = wsl::shared::FromJson<DistributionList>(content.c_str());

        if (distros.Distributions.has_value())
        {
            std::erase_if(*distros.Distributions, [](const auto& e) {
                if constexpr (wsl::shared::Arm64)
                {
                    return !e.Arm64;
                }
                else
                {
                    return !e.Amd64;
                }
            });
        }

        if (distros.ModernDistributions.has_value())
        {
            for (auto& [_, versions] : *distros.ModernDistributions)
            {
                std::erase_if(versions, [](const auto& e) {
                    if constexpr (wsl::shared::Arm64)
                    {
                        return !e.Arm64Url.has_value();
                    }
                    else
                    {
                        return !e.Amd64Url.has_value();
                    }
                });
            }
        }

        // The "Default" string takes precedence. If not present, use the first legacy distro entry.
        if (!distros.Default.has_value() && distros.Distributions.has_value() && distros.Distributions->size() > 0)
        {
            distros.Default = (*distros.Distributions)[0].Name;
        }

        return distros;
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        THROW_HR_WITH_USER_ERROR(
            hr, wsl::shared::Localization::MessageCouldFetchDistributionList(url.c_str(), wsl::windows::common::wslutil::GetSystemErrorString(hr)));
    }
}

std::optional<TDistribution> LookupDistributionInManifest(const DistributionList& manifest, LPCWSTR name, bool legacy)
{
    // First check if the name matches a distribution, or a distribution version in the modern entries

    const auto utf8name = wsl::shared::string::WideToMultiByte(name);
    if (!legacy && manifest.ModernDistributions.has_value())
    {
        for (const auto& [distributionName, versions] : *manifest.ModernDistributions)
        {
            bool useDefault = false;
            if (wsl::shared::string::IsEqual(distributionName, utf8name, true))
            {
                useDefault = true;
            }

            for (const auto& e : versions)
            {
                if (useDefault && e.Default.value_or(false))
                {
                    return e;
                }

                if (wsl::shared::string::IsEqual(e.Name, name, true))
                {
                    return e;
                }
            }
        }
    }

    // If no modern distribution is found, or --legacy is passed, look for a legacy registration

    if (!manifest.Distributions.has_value())
    {
        return {};
    }

    const auto it = std::find_if(manifest.Distributions->begin(), manifest.Distributions->end(), [&](const auto e) {
        return wsl::shared::string::IsEqual(e.Name, name, true);
    });

    if (it == manifest.Distributions->end())
    {
        return {};
    }

    return *it;
}

// Helper function to merge distributions from multiple manifests
void MergeDistributionLists(DistributionList& target, const DistributionList& source)
{
    // Merge legacy distributions
    if (source.Distributions.has_value())
    {
        if (!target.Distributions.has_value())
        {
            target.Distributions = std::vector<Distribution>{};
        }
        
        for (const auto& dist : *source.Distributions)
        {
            // Check if distribution already exists (avoid duplicates)
            auto it = std::find_if(target.Distributions->begin(), target.Distributions->end(),
                [&](const Distribution& d) { return d.Name == dist.Name; });
            
            if (it == target.Distributions->end())
            {
                target.Distributions->push_back(dist);
            }
        }
    }

    // Merge modern distributions
    if (source.ModernDistributions.has_value())
    {
        if (!target.ModernDistributions.has_value())
        {
            target.ModernDistributions = std::map<std::string, std::vector<ModernDistributionVersion>>{};
        }

        for (const auto& [distroName, versions] : *source.ModernDistributions)
        {
            auto& targetVersions = (*target.ModernDistributions)[distroName];
            
            for (const auto& version : versions)
            {
                // Check if version already exists
                auto it = std::find_if(targetVersions.begin(), targetVersions.end(),
                    [&](const ModernDistributionVersion& v) { return v.Name == version.Name; });
                
                if (it == targetVersions.end())
                {
                    targetVersions.push_back(version);
                }
            }
        }
    }

    // Update default if source has one and target doesn't
    if (source.Default.has_value() && !target.Default.has_value())
    {
        target.Default = source.Default;
    }
}

} // namespace

AvailableDistributions wsl::windows::common::distribution::GetAvailable()
{
    AvailableDistributions distributions{};

    // Determine the base manifest URL
    // Priority: HKCU > HKLM > Default
    std::wstring url = c_defaultDistroListUrl;
    std::vector<std::wstring> appendUrls;

    try
    {
        // First check HKEY_LOCAL_MACHINE
        const auto machineKey = registry::OpenLxssMachineKey();
        url = registry::ReadString(machineKey.get(), nullptr, c_distroUrlRegistryValue, c_defaultDistroListUrl);
        
        // Read HKLM append URLs (supports REG_MULTI_SZ)
        auto hklmAppendUrls = registry::ReadWideStringSet(machineKey.get(), nullptr, c_distroUrlAppendRegistryValue, {});
        appendUrls.insert(appendUrls.end(), hklmAppendUrls.begin(), hklmAppendUrls.end());
        
        if (url != c_defaultDistroListUrl)
        {
            WSL_LOG("Found custom URL for distribution list in HKLM", TraceLoggingValue(url.c_str(), "url"));
        }
        
        if (!appendUrls.empty())
        {
            WSL_LOG("Found append URLs in HKLM", TraceLoggingValue(static_cast<UINT32>(appendUrls.size()), "count"));
        }
    }
    CATCH_LOG()

    try
    {
        // Then check HKEY_CURRENT_USER (takes precedence)
        const auto userKey = registry::OpenLxssUserKey();
        
        // Check if user has overridden the base URL
        auto userUrl = registry::ReadOptionalString(userKey.get(), nullptr, c_distroUrlRegistryValue);
        if (userUrl.has_value())
        {
            url = userUrl.value();
            WSL_LOG("Found custom URL for distribution list in HKCU (overriding)", TraceLoggingValue(url.c_str(), "url"));
        }
        
        // Read HKCU append URLs (supports REG_MULTI_SZ) - these are added to HKLM append URLs
        auto hkcuAppendUrls = registry::ReadWideStringSet(userKey.get(), nullptr, c_distroUrlAppendRegistryValue, {});
        appendUrls.insert(appendUrls.end(), hkcuAppendUrls.begin(), hkcuAppendUrls.end());
        
        if (!hkcuAppendUrls.empty())
        {
            WSL_LOG("Found append URLs in HKCU", TraceLoggingValue(static_cast<UINT32>(hkcuAppendUrls.size()), "count"));
        }
    }
    CATCH_LOG()

    // Load the base manifest
    distributions.Manifest = ReadFromManifest(url);

    // Load and merge all append manifests
    if (!appendUrls.empty())
    {
        for (const auto& appendUrl : appendUrls)
        {
            try
            {
                WSL_LOG("Loading append manifest", TraceLoggingValue(appendUrl.c_str(), "url"));
                auto appendManifest = ReadFromManifest(appendUrl);
                
                // Merge into override manifest if it exists, otherwise create it
                if (!distributions.OverrideManifest.has_value())
                {
                    distributions.OverrideManifest = appendManifest;
                }
                else
                {
                    MergeDistributionLists(*distributions.OverrideManifest, appendManifest);
                }
            }
            catch (...)
            {
                // Log the error but continue with other sources
                LOG_CAUGHT_EXCEPTION_MSG("Failed to load append manifest from %ls", appendUrl.c_str());
            }
        }
    }

    return distributions;
}

std::variant<Distribution, ModernDistributionVersion> wsl::windows::common::distribution::LookupByName(
    const AvailableDistributions& manifest, LPCWSTR name, bool legacy)
{
    if (manifest.OverrideManifest.has_value())
    {
        auto distribution = LookupDistributionInManifest(manifest.OverrideManifest.value(), name, legacy);
        if (distribution.has_value())
        {
            EMIT_USER_WARNING(wsl::shared::Localization::MessageDistributionOverridden(name));
            return distribution.value();
        }
    }

    auto distribution = LookupDistributionInManifest(manifest.Manifest, name, legacy);

    if (!distribution.has_value())
    {
        THROW_HR_WITH_USER_ERROR(WSL_E_DISTRO_NOT_FOUND, Localization::MessageInvalidDistributionName(name));
    }

    return distribution.value();
}

bool wsl::windows::common::distribution::IsInstalled(const Distribution& distro, bool directDownload)
{
    const auto familyName = GetFamilyName(distro, directDownload);
    return GetInstalledPackage(familyName.c_str()).has_value();
}

void wsl::windows::common::distribution::LegacyInstallViaGithub(const Distribution& distro)
{
    decltype(distro.Amd64PackageUrl) downloadUrl;

    if constexpr (wsl::shared::Arm64)
    {
        downloadUrl = distro.Arm64PackageUrl;
    }
    else
    {
        downloadUrl = distro.Amd64PackageUrl;
    }

    THROW_HR_IF(WSL_E_DISTRO_ONLY_AVAILABLE_FROM_STORE, !downloadUrl.has_value());

    wslutil::PrintMessage(Localization::MessageDownloading(distro.FriendlyName.c_str()), stdout);

    // Note: The appx extensions is required for the installation to succeed.
    const auto downloadPath = wslutil::DownloadFile(*downloadUrl, distro.Name + L".appx");
    auto deleteFile =
        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] { THROW_IF_WIN32_BOOL_FALSE(DeleteFileW(downloadPath.c_str())); });

    wslutil::PrintMessage(Localization::MessageInstalling(distro.FriendlyName), stdout);

    const winrt::Windows::Management::Deployment::PackageManager packageManager;
    auto installedPackage = packageManager
                                .AddPackageAsync(
                                    winrt::Windows::Foundation::Uri{downloadPath},
                                    nullptr,
                                    winrt::Windows::Management::Deployment::DeploymentOptions::None,
                                    wsl::windows::common::wslutil::GetSystemVolume())
                                .get();

    wslutil::PrintMessage(Localization::MessageDownloadComplete(distro.FriendlyName), stdout);
}

void wsl::windows::common::distribution::LegacyInstallViaStore(const Distribution& distro)
{
    const AppInstallOptions options;
    options.CompletedInstallToastNotificationMode(AppInstallationToastNotificationMode::NoToast);

    const AppInstallManager manager;
    const auto entries =
        manager.StartProductInstallAsync(distro.StoreAppId.c_str(), winrt::hstring{}, StoreClientId, winrt::hstring{}, options).get();

    // Cancel the app deployment if something goes wrong
    auto cancel = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
        for (const auto& e : entries)
        {
            e.Cancel();
        }
    });

    wslutil::PrintMessage(Localization::MessageDownloading(distro.FriendlyName), stdout);

    // Print install progress.
    auto complete = [&]() {
        for (uint32_t i = 0; i < entries.Size(); i++)
        {
            if (entries.GetAt(i).GetCurrentStatus().InstallState() != AppInstallState::Completed)
            {
                return false;
            }
        }
        return true;
    };

    ConsoleProgressBar progressBar;
    const auto total = std::lround(100 * entries.Size());
    while (!complete())
    {
        double percentComplete = 0;
        for (const auto& e : entries)
        {
            const auto& status = e.GetCurrentStatus();
            THROW_IF_FAILED(static_cast<HRESULT>(status.ErrorCode()));

            percentComplete += status.PercentComplete();
        }

        progressBar.Print(static_cast<UINT>(percentComplete), total);
        Sleep(100);
    }

    progressBar.Clear();
    cancel.release();

    wslutil::PrintMessage(Localization::MessageDownloadComplete(distro.FriendlyName), stdout);

    // Sanity check
    THROW_HR_IF(E_UNEXPECTED, !IsInstalled(distro, false));
}

void wsl::windows::common::distribution::Launch(const Distribution& distro, bool directDownload, bool throwOnError)
{
    const std::wstring familyName = GetFamilyName(distro, directDownload);

    try
    {
        wil::unique_cotaskmem_string appsPath;
        THROW_IF_FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_NO_APPCONTAINER_REDIRECTION, nullptr, &appsPath));

        const std::filesystem::path appsFolder{appsPath.get()};

        std::optional<std::filesystem::path> entryPoint;
        for (const auto& e : std::filesystem::directory_iterator(appsFolder / L"Microsoft" / "WindowsApps" / familyName))
        {
            if (e.path().has_extension() &&
                wsl::windows::common::string::IsPathComponentEqual(e.path().extension().native(), L".exe"))
            {
                if (entryPoint.has_value())
                {
                    // Note: Can't use THROW_HR_IF* here because entryPoint.value() should only be called if entryPoint has a value.
                    THROW_HR_MSG(
                        E_UNEXPECTED,
                        "Found multiple entrypoints for app: %ls (%ls, %ls), falling back to LaunchAsync()",
                        familyName.c_str(),
                        entryPoint.value().c_str(),
                        e.path().c_str());
                }

                entryPoint = e.path();
            }
        }

        THROW_HR_IF_MSG(
            E_UNEXPECTED,
            !entryPoint.has_value(),
            "No entrypoint found for app: %ls, path: %ls",
            familyName.c_str(),
            (appsFolder / L"Microsoft" / "WindowsApps" / familyName).c_str());

        auto commandLine = entryPoint->wstring();
        const auto exitCode = wsl::windows::common::helpers::RunProcess(commandLine);
        if (throwOnError && exitCode != 0)
        {
            THROW_HR_WITH_USER_ERROR(WSL_E_INSTALL_PROCESS_FAILED, wsl::shared::Localization::MessageInstallProcessFailed(distro.Name, exitCode));
        }
        return;
    }
    catch (...)
    {
        if (wil::ResultFromCaughtException() == WSL_E_INSTALL_PROCESS_FAILED)
        {
            throw;
        }
        else
        {
            LOG_CAUGHT_EXCEPTION();
        }
    }

    // Fallback to the old launch logic in case something went wrong looking up the app execution alias.

    const auto package = GetInstalledPackage(familyName.c_str());
    THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_INVALID_STATE), !package.has_value());

    const auto entryPoints = package->GetAppListEntries();
    THROW_HR_IF_MSG(
        E_UNEXPECTED,
        entryPoints.Size() != 1,
        "Unexpected number of entry points for app: %ls, %i",
        distro.StoreAppId.c_str(),
        !entryPoints.Size());

    entryPoints.GetAt(0).LaunchAsync().get();
}
