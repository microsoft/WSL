/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslInstall.h

Abstract:

    This file contains the definitions for installing WSL distributions

--*/

#pragma once
#include <functional>
#include <string_view>

#include "Distribution.h"
#include "OptionalFeature.h"

class WslInstall
{
public:
    static inline LPCWSTR c_optionalFeatureNameVmp = L"VirtualMachinePlatform";
    static inline LPCWSTR c_optionalFeatureNameWsl = L"Microsoft-Windows-Subsystem-Linux";

    struct InstallResult
    {
        std::wstring Name;
        std::optional<GUID> Id;
        std::optional<wsl::windows::common::distribution::TDistribution> Distribution;
        bool InstalledViaGitHub{};
        bool Alreadyinstalled{};
    };

    static HRESULT InstallDistribution(
        _Out_ InstallResult& installResult,
        _In_ const std::optional<std::wstring>& distributionName,
        _In_ const std::optional<ULONG>& version,
        _In_ bool launchAfterInstall,
        _In_ bool useGitHub,
        _In_ bool legacy,
        _In_ bool fixedVhd,
        _In_ const std::optional<std::wstring>& localName,
        _In_ const std::optional<std::wstring>& location,
        _In_ const std::optional<uint64_t>& vhdSize);

    struct OptionalComponentRequirements
    {
        bool RebootRequired{};
        std::vector<std::wstring> ComponentsToEnable;
    };

    using OptionalFeatureStateQuery = std::function<wsl::windows::common::optionalfeature::State(std::wstring_view featureName)>;

    static OptionalComponentRequirements CheckForMissingOptionalComponents(_In_ bool requireWslOptionalComponent);

    static OptionalComponentRequirements CheckForMissingOptionalComponents(_In_ bool requireWslOptionalComponent, const OptionalFeatureStateQuery& queryFeatureState);

    static OptionalComponentRequirements EvaluateOptionalComponentRequirements(
        _In_ bool requireWslOptionalComponent, const OptionalFeatureStateQuery& queryFeatureState);

    static void InstallOptionalComponents(const std::vector<std::wstring>& components);

    static DWORD InstallOptionalComponent(LPCWSTR component, bool consoleOutput);

    static std::pair<std::wstring, GUID> InstallModernDistribution(
        const wsl::windows::common::distribution::ModernDistributionVersion& distribution,
        const std::optional<ULONG>& version,
        const std::optional<std::wstring>& name,
        const std::optional<std::wstring>& location,
        const std::optional<uint64_t>& vhdSize,
        const bool fixedVhd);
};
