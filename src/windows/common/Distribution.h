/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Distribution.cpp

Abstract:

    This file contains definitions for distribution app download, install and launch.

--*/

#pragma once

#include <optional>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>
#include "JsonUtils.h"

namespace wsl::windows::common::distribution {

struct DistributionArchive
{
    std::wstring Url;
    std::wstring Sha256;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DistributionArchive, Url, Sha256);
};

struct ModernDistributionVersion
{
    std::wstring Name;
    std::wstring FriendlyName;
    std::optional<bool> Default;
    std::optional<DistributionArchive> Amd64Url;
    std::optional<DistributionArchive> Arm64Url;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ModernDistributionVersion, Name, FriendlyName, Default, Amd64Url, Arm64Url);
};

struct Distribution
{
    std::wstring Name;
    std::wstring FriendlyName;
    std::wstring StoreAppId;
    bool Amd64;
    bool Arm64;
    std::optional<std::wstring> Amd64PackageUrl;
    std::optional<std::wstring> Arm64PackageUrl;
    std::optional<std::wstring> PackageFamilyName;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Distribution, Name, FriendlyName, StoreAppId, Amd64, Arm64, Amd64PackageUrl, Arm64PackageUrl, PackageFamilyName);
};

struct DistributionList
{
    std::optional<std::vector<Distribution>> Distributions;
    std::optional<std::map<std::string, std::vector<ModernDistributionVersion>>> ModernDistributions;
    std::optional<std::wstring> Default;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DistributionList, Distributions, ModernDistributions, Default)
};

constexpr inline auto c_distroUrlRegistryValue = L"DistributionListUrl";
constexpr inline auto c_distroUrlAppendRegistryValue = L"DistributionListUrlAppend";
constexpr inline auto fileUrlPrefix = L"file://";

using TDistribution = std::variant<Distribution, ModernDistributionVersion>;

struct AvailableDistributions
{
    DistributionList Manifest;
    std::optional<DistributionList> OverrideManifest;
};

AvailableDistributions GetAvailable();
TDistribution LookupByName(const AvailableDistributions& distributions, LPCWSTR name, bool legacy);

bool IsInstalled(const Distribution& distro, bool directDownload);

void LegacyInstallViaStore(const Distribution& distro);

void LegacyInstallViaGithub(const Distribution& distro);

void Launch(const Distribution& distro, bool directDownload, bool throwOnError);

} // namespace wsl::windows::common::distribution
