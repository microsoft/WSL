/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    WslInstaller.cpp

Abstract:

    This file contains the implementation of the WslInstaller class.

--*/

#include "precomp.h"
#include "WslInstaller.h"

extern wil::unique_event g_stopEvent;

std::wstring GetMsiPackagePath()
{
#ifdef WSL_DEV_THIN_MSI_PACKAGE

    static_assert(!wsl::shared::OfficialBuild);

    return std::filesystem::weakly_canonical(WSL_DEV_THIN_MSI_PACKAGE).wstring();

#endif

    return (wsl::windows::common::wslutil::GetBasePath() / L"wsl.msi").wstring();
}

std::optional<std::wstring> GetUpgradeLogFileLocation()
try
{
    const auto key = wsl::windows::common::registry::OpenLxssMachineKey();
    const auto path = wsl::windows::common::registry::ReadString(key.get(), L"MSI", L"UpgradeLogFile", L"");
    if (path.empty())
    {
        return {};
    }

    // A canonical path is required because msiexec doesn't like symlinks.
    return std::filesystem::weakly_canonical(path);
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();
    return {};
}

std::pair<UINT, std::wstring> InstallMsipackageImpl()
{
    const auto logFile = GetUpgradeLogFileLocation();

    std::wstring errors;
    auto messageCallback = [&errors](INSTALLMESSAGE type, LPCWSTR message) {
        switch (type)
        {
        case INSTALLMESSAGE_ERROR:
        case INSTALLMESSAGE_FATALEXIT:
        case INSTALLMESSAGE_WARNING:
        case INSTALLMESSAGE_FILESINUSE:
        case INSTALLMESSAGE_OUTOFDISKSPACE:
            if (!errors.empty())
            {
                errors += L"\n";
            }

            errors += message;
            break;

        default:
            break;
        }
    };

    auto result = wsl::windows::common::wslutil::UpgradeViaMsi(
        GetMsiPackagePath().c_str(), L"SKIPMSIX=1", logFile.has_value() ? logFile->c_str() : nullptr, messageCallback);

    WSL_LOG("MSIUpgradeResult", TraceLoggingValue(result, "result"), TraceLoggingValue(errors.c_str(), "errorMessage"));

    return {result, errors};
}

DWORD WINAPI InstallMsiPackage(LPVOID Context)
{
    auto* installContext = reinterpret_cast<InstallContext*>(Context);

    try
    {
        std::tie(installContext->ExitCode, installContext->Errors) = InstallMsipackageImpl();
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        installContext->Result = wil::ResultFromCaughtException();
        return 0;
    }

    installContext->Result = S_OK;
    return 0;
}

std::pair<bool, std::wstring> IsUpdateNeeded()
{
    try
    {
        const auto key = wsl::windows::common::registry::OpenLxssMachineKey();

        const auto installedVersion = wsl::windows::common::registry::ReadString(key.get(), L"MSI", L"Version", L"");

        WSL_LOG(
            "DetectedInstalledVersion",
            TraceLoggingLevel(WINEVENT_LEVEL_INFO),
            TraceLoggingValue(installedVersion.c_str(), "InstalledVersion"));

        return std::make_pair(
            installedVersion.empty() || wsl::windows::common::wslutil::ParseWslPackageVersion(installedVersion) < wsl::shared::PackageVersion,
            installedVersion);
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();

        return std::make_pair(false, L"");
    }
}

std::shared_ptr<InstallContext> LaunchInstall()
{
    static wil::srwlock mutex;
    static std::weak_ptr<InstallContext> weak_context;

    auto lock = mutex.lock_exclusive();

    auto [updateNeeded, existingVersion] = IsUpdateNeeded();
    if (!updateNeeded)
    {
        return {};
    }

    wsl::windows::common::wslutil::WriteInstallLog(std::format("Starting upgrade via WslInstaller. Previous version: {}", existingVersion));

    // Return an existing install if any
    if (auto ptr = weak_context.lock(); ptr != nullptr)
    {
        return ptr;
    }

    // Else launch a new install
    auto context = std::make_shared<InstallContext>();
    weak_context = std::weak_ptr<InstallContext>(context);

    context->Thread = wil::unique_handle{CreateThread(nullptr, 0, &InstallMsiPackage, context.get(), 0, nullptr)};

    return context;
}

HRESULT WslInstaller::Install(UINT* ExitCode, LPWSTR* Errors)
try
{
    const auto context = LaunchInstall();
    if (!context)
    {
        // This block can be reached if the installation completed after the client looked up the MSI package.
        // In this case don't attempt to install and return success so the client looks up the MSI package again.

        *ExitCode = 0;
        *Errors = wil::make_unique_string<wil::unique_cotaskmem_string>(L"").release();
        return S_OK;
    }

    THROW_LAST_ERROR_IF(WaitForSingleObject(context->Thread.get(), INFINITE) != WAIT_OBJECT_0);

    *ExitCode = context->ExitCode;
    *Errors = wil::make_unique_string<wil::unique_cotaskmem_string>(context->Errors.c_str()).release();

    return context->Result;
}
CATCH_RETURN()