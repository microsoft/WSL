/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DllMain.cpp

Abstract:

    This file the entrypoint for the WSLA client library.

--*/

#include "precomp.h"
#include "wslaservice.h"
#include "WSLAApi.h"
#include "wslrelay.h"
#include "wslInstall.h"

EXTERN_C BOOL STDAPICALLTYPE DllMain(_In_ HINSTANCE Instance, _In_ DWORD Reason, _In_opt_ LPVOID Reserved)
{
    wil::DLLMain(Instance, Reason, Reserved);

    switch (Reason)
    {
    case DLL_PROCESS_ATTACH:
        WslTraceLoggingInitialize(LxssTelemetryProvider, false);
        wsl::windows::common::wslutil::InitializeWil();

        break;

    case DLL_PROCESS_DETACH:
        WslTraceLoggingUninitialize();
        break;
    }

    return TRUE;
}

DEFINE_ENUM_FLAG_OPERATORS(WslInstallComponent);

HRESULT WslQueryMissingComponents(enum WslInstallComponent* Components)
try
{
    *Components = WslInstallComponentNone;

    // Check for Windows features
    WI_SetFlagIf(
        *Components,
        WslInstallComponentWslOC,
        !wsl::windows::common::helpers::IsWindows11OrAbove() && !wsl::windows::common::helpers::IsServicePresent(L"lxssmanager"));

    WI_SetFlagIf(*Components, WslInstallComponentVMPOC, !wsl::windows::common::wslutil::IsVirtualMachinePlatformInstalled());

    // Check if the WSL package is installed, and if the version supports WSLA
    auto version = wsl::windows::common::wslutil::GetInstalledPackageVersion();

    constexpr auto minimalPackageVersion = std::tuple<uint32_t, uint32_t, uint32_t>{2, 7, 0};
    WI_SetFlagIf(*Components, WslInstallComponentWslPackage, !version.has_value() || version < minimalPackageVersion);

    // TODO: Check if hardware supports virtualization.

    return S_OK;
}
CATCH_RETURN();

// Used for debugging.
static LPCWSTR PackageUrl = nullptr;

HRESULT WslSetPackageUrl(LPCWSTR Url)
{
    PackageUrl = Url;
    return S_OK;
}

HRESULT WslInstallComponents(enum WslInstallComponent Components, WslInstallCallback ProgressCallback, void* Context)
try
{
    // Check for invalid flags.
    RETURN_HR_IF_MSG(
        E_INVALIDARG,
        (Components & ~(WslInstallComponentVMPOC | WslInstallComponentWslOC | WslInstallComponentWslPackage)) != 0,
        "Unexpected flag: %i",
        Components);

    // Fail if the caller is not elevated.
    RETURN_HR_IF(
        HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED),
        Components != 0 && !wsl::windows::common::security::IsTokenElevated(wil::open_current_access_token().get()));

    if (WI_IsFlagSet(Components, WslInstallComponentWslPackage))
    {
        THROW_HR_IF(E_INVALIDARG, PackageUrl == nullptr);

        auto callback = [&](uint64_t progress, uint64_t total) {
            if (ProgressCallback != nullptr)
            {
                ProgressCallback(WslInstallComponentWslPackage, progress, total, Context);
            }
        };

        const auto downloadPath = wsl::windows::common::wslutil::DownloadFileImpl(PackageUrl, L"wsla.msi", callback);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { std::filesystem::remove(downloadPath); });

        auto exitCode = wsl::windows::common::wslutil::UpgradeViaMsi(downloadPath.c_str(), nullptr, nullptr, [](auto, auto) {});
        THROW_HR_IF_MSG(
            E_FAIL, exitCode != 0, "MSI installation failed. URL: %ls, DownloadPath: %ls, exitCode: %u", downloadPath.c_str(), PackageUrl, exitCode);
    }

    std::vector<std::wstring> optionalComponents;
    if (WI_IsFlagSet(Components, WslInstallComponentWslOC))
    {
        if (ProgressCallback != nullptr)
        {
            ProgressCallback(WslInstallComponentWslOC, 0, 1, Context);
        }

        auto exitCode = WslInstall::InstallOptionalComponent(WslInstall::c_optionalFeatureNameWsl, false);
        THROW_HR_IF_MSG(E_FAIL, exitCode != 0 && exitCode != ERROR_SUCCESS_REBOOT_REQUIRED, "Failed to install '%ls', %lu", WslInstall::c_optionalFeatureNameWsl, exitCode);
    }

    if (WI_IsFlagSet(Components, WslInstallComponentVMPOC))
    {
        if (ProgressCallback != nullptr)
        {
            ProgressCallback(WslInstallComponentVMPOC, 0, 1, Context);
        }

        auto exitCode = WslInstall::InstallOptionalComponent(WslInstall::c_optionalFeatureNameVmp, false);
        THROW_HR_IF_MSG(E_FAIL, exitCode != 0 && exitCode != ERROR_SUCCESS_REBOOT_REQUIRED, "Failed to install '%ls', %lu", WslInstall::c_optionalFeatureNameVmp, exitCode);
    }

    return WI_IsAnyFlagSet(Components, WslInstallComponentWslOC | WslInstallComponentVMPOC)
               ? HRESULT_FROM_WIN32(ERROR_SUCCESS_REBOOT_REQUIRED)
               : S_OK;
}
CATCH_RETURN();