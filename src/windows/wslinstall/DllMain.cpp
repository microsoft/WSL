/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DllMain.cpp

Abstract:

    This file contains various methods used during MSI installation (see package.wix.in)

--*/

#include "precomp.h"
#include <msiquery.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/windows.management.deployment.h>
#include <Sfc.h>
#include "defs.h"

using unique_msi_handle = wil::unique_any<MSIHANDLE, decltype(MsiCloseHandle), &MsiCloseHandle>;

using namespace wsl::windows::common::registry;
using namespace wsl::windows::common::wslutil;

static constexpr auto c_progIdPrefix{L"App."};
static constexpr auto c_protocolProgIdSuffix{L".Protocol"};
static constexpr auto c_wslSettingsInstalledDirectoryPropertyName = L"WSLSETTINGS";
static constexpr auto c_wslSettingsAppIDPropertyName = L"WSLSETTINGSAPPID";
static constexpr auto c_wslSettingsProgIDPropertyName = L"WSLSETTINGSPROGID";

#define IGNORE_MSIX_ERROR_IF_DIRECT_MSI_EXECUTION_SUPPORTED() \
    if (DoesBuildSupportDirectMsiExecution()) \
    { \
        WSL_LOG( \
            "IgnoredMsixError", \
            TraceLoggingValue(wil::ResultFromCaughtException(), "Error"), \
            TraceLoggingValue(__FUNCTION__, "Stage")); \
\
        return NOERROR; \
    }

#ifndef WSL_OFFICIAL_BUILD
void TrustPackageCertificate(LPCWSTR Path)
{
    wil::unique_hcertstore store;
    wil::unique_hcryptmsg msg;

    WSL_LOG("TrustMSIXCertificate", TraceLoggingValue(Path, "Path"));

    // Retrieve the certificate from the MSIX
    THROW_IF_WIN32_BOOL_FALSE(CryptQueryObject(
        CERT_QUERY_OBJECT_FILE, Path, CERT_QUERY_CONTENT_FLAG_ALL, CERT_QUERY_FORMAT_FLAG_ALL, 0, nullptr, nullptr, nullptr, &store, &msg, nullptr));

    const wil::unique_cert_context cert{
        CertFindCertificateInStore(store.get(), X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_ANY, nullptr, nullptr)};
    THROW_LAST_ERROR_IF(!cert);

    const wil::unique_hcertstore trustedRoot{
        CertOpenStore(CERT_STORE_PROV_SYSTEM, 0, 0, CERT_STORE_OPEN_EXISTING_FLAG | CERT_SYSTEM_STORE_LOCAL_MACHINE, L"ROOT")};

    THROW_LAST_ERROR_IF(!trustedRoot);

    THROW_IF_WIN32_BOOL_FALSE(CertAddCertificateContextToStore(trustedRoot.get(), cert.get(), CERT_STORE_ADD_USE_EXISTING, nullptr));
}
#endif

void ThrowIfOperationError(
    const winrt::Windows::Foundation::IAsyncOperationWithProgress<winrt::Windows::Management::Deployment::DeploymentResult, winrt::Windows::Management::Deployment::DeploymentProgress>& result,
    const std::source_location& source = std::source_location::current())
{
    const auto status = result.get();

    if (result.Status() == winrt::Windows::Foundation::AsyncStatus::Error)
    {
        THROW_HR_MSG(result.ErrorCode(), "Source: %hs() - %hs:%lu", source.function_name(), source.file_name(), source.line());
    }

    THROW_IF_FAILED_MSG(status.ExtendedErrorCode(), "%ls", status.ErrorText().c_str());
}

std::wstring GetMsiProperty(MSIHANDLE install, LPCWSTR name)
{
    DWORD size{};
    std::wstring output(1, '\0');
    UINT result = MsiGetProperty(install, name, output.data(), &size);
    THROW_HR_IF_MSG(E_UNEXPECTED, result != ERROR_SUCCESS && result != ERROR_MORE_DATA, "MsiGetProperty failed with %u", result);

    output.resize(size);
    size = static_cast<DWORD>(output.size() + 1);
    result = MsiGetProperty(install, name, output.data(), &size);
    THROW_HR_IF_MSG(E_UNEXPECTED, result != ERROR_SUCCESS, "MsiGetProperty for '%s' failed with %u", name, result);

    WI_ASSERT(size == output.size());
    return output;
}

std::wstring GetInstallTarget(MSIHANDLE install)
{
    return GetMsiProperty(install, L"CustomActionData");
}

void DisplayError(MSIHANDLE install, LPCWSTR message)
{
    const unique_msi_handle record{MsiCreateRecord(0)};
    MsiRecordSetString(record.get(), 0, message);

    MsiProcessMessage(install, INSTALLMESSAGE(INSTALLMESSAGE_ERROR + MB_OK), record.get());
}

void DeleteRegistryKeyIfVolatile(LPCWSTR Parent, LPCWSTR Key)
{

    const std::wstring path = Parent + std::wstring(L"\\") + Key;
    auto [key, error] = wsl::windows::common::registry::OpenKeyNoThrow(HKEY_LOCAL_MACHINE, path.c_str(), KEY_READ);

    if (FAILED(error))
    {
        THROW_HR_IF(error, error != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && error != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));

        // The key doesn't exist, nothing to do.
        return;
    }

    if (!wsl::windows::common::registry::IsKeyVolatile(key.get()))
    {
        // Registry key is not volatile, nothing to do.
        return;
    }

    WSL_LOG("CleanMsixRegistryKeys", TraceLoggingValue(Parent, "Parent"), TraceLoggingValue(Key, "Key"));

    const auto parent = wsl::windows::common::registry::OpenKey(HKEY_LOCAL_MACHINE, Parent, KEY_ALL_ACCESS);
    wsl::windows::common::registry::DeleteKey(parent.get(), Key);
}

bool IsWindowsServerCore()
{
    wil::unique_hkey key;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Server\\ServerLevels", 0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        // NanoServer must be 1, or ServerCore must be 1, Server-Gui-Mgmt must be zero or not present, and Server-Gui-Shell must be zero or not present
        DWORD value = 0;
        DWORD valueSize = sizeof(value);
        if ((RegGetValue(key.get(), nullptr, L"NanoServer", RRF_RT_REG_DWORD, nullptr, &value, &valueSize) == ERROR_SUCCESS) && (value == 1))
        {
            return true;
        }
        else
        {
            value = 0;
            valueSize = sizeof(value);
            if ((RegGetValue(key.get(), nullptr, L"ServerCore", RRF_RT_REG_DWORD, nullptr, &value, &valueSize) == ERROR_SUCCESS) &&
                (value == 1))
            {
                value = 0;
                valueSize = sizeof(value);
                RegGetValue(key.get(), nullptr, L"Server-Gui-Mgmt", (RRF_RT_REG_DWORD | RRF_ZEROONFAILURE), nullptr, &value, &valueSize);
                if (value == 0)
                {
                    value = 0;
                    valueSize = sizeof(value);
                    RegGetValue(key.get(), nullptr, L"Server-Gui-Shell", (RRF_RT_REG_DWORD | RRF_ZEROONFAILURE), nullptr, &value, &valueSize);
                    return value == 0;
                }
            }
        }
    }

    return false;
}

bool IsWindowsServerCoreWithMsiSupport()
{
    return IsWindowsServerCore() && wsl::windows::common::helpers::GetWindowsVersion().BuildNumber >=
                                        wsl::windows::common::helpers::WindowsBuildNumbers::Germanium;
}

bool DoesBuildSupportDirectMsiExecution()
{
    const auto buildInfo = wsl::windows::common::helpers::GetWindowsVersion();

    switch (buildInfo.BuildNumber)
    {

    // For Windows 10, the fix was only serviced to 22h2 and 21h2.
    case wsl::windows::common::helpers::WindowsBuildNumbers::Vibranium_21H2:
        return buildInfo.UpdateBuildRevision >= 4529;

    case wsl::windows::common::helpers::WindowsBuildNumbers::Vibranium_22H2:
        return buildInfo.UpdateBuildRevision >= 4474;

    case wsl::windows::common::helpers::WindowsBuildNumbers::Iron:
        return buildInfo.UpdateBuildRevision >= 2582;

    case wsl::windows::common::helpers::WindowsBuildNumbers::Cobalt:
        return false; // cobalt builds aren't serviced anymore, so the fix wasn't backported there.

    case wsl::windows::common::helpers::WindowsBuildNumbers::Nickel:
    case wsl::windows::common::helpers::WindowsBuildNumbers::Nickel_23H2: // See: https://learn.microsoft.com/en-us/windows/release-health/windows11-release-information
        return buildInfo.UpdateBuildRevision >= 3672;

    case wsl::windows::common::helpers::WindowsBuildNumbers::Zinc:
        return buildInfo.UpdateBuildRevision >= 1009;

    default:
        return buildInfo.BuildNumber >= wsl::windows::common::helpers::WindowsBuildNumbers::Germanium;
    }
}

void GrantDeletePermissionToSystem(SC_HANDLE Service)
{
    // Get the current security descriptor
    DWORD bytesNeeded{};
    THROW_LAST_ERROR_IF(!QueryServiceObjectSecurity(Service, DACL_SECURITY_INFORMATION, nullptr, 0, &bytesNeeded) && GetLastError() != ERROR_INSUFFICIENT_BUFFER);

    std::vector<char> buffer(bytesNeeded);
    THROW_IF_WIN32_BOOL_FALSE(
        QueryServiceObjectSecurity(Service, DACL_SECURITY_INFORMATION, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesNeeded));

    // Get the DACL.
    PACL previousAcl{};
    BOOL present{};
    BOOL defaulted{};
    THROW_IF_WIN32_BOOL_FALSE(GetSecurityDescriptorDacl(buffer.data(), &present, &previousAcl, &defaulted));

    // Build a new ACE for SYSTEM.
    EXPLICIT_ACCESS access{};
    std::wstring account = L"SYSTEM";
    BuildExplicitAccessWithName(&access, account.data(), DELETE, SET_ACCESS, NO_INHERITANCE);

    // Create a new ACL with the new ACE.
    wsl::windows::common::security::unique_acl newAcl;

    THROW_IF_WIN32_ERROR(SetEntriesInAcl(1, &access, previousAcl, &newAcl));

    // Build a new security descriptor with that ACL.
    SECURITY_DESCRIPTOR newDescriptor{};
    THROW_IF_WIN32_BOOL_FALSE(InitializeSecurityDescriptor(&newDescriptor, SECURITY_DESCRIPTOR_REVISION));
    THROW_IF_WIN32_BOOL_FALSE(SetSecurityDescriptorDacl(&newDescriptor, true, newAcl.get(), false));

    // Update the service's ACL.
    THROW_IF_WIN32_BOOL_FALSE(SetServiceObjectSecurity(Service, DACL_SECURITY_INFORMATION, &newDescriptor));
}

void RemoveMsixService()
try
{
    const wil::unique_schandle manager{OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS)};
    THROW_LAST_ERROR_IF(!manager);

    wil::unique_schandle wslservice{OpenService(manager.get(), L"wslservice", READ_CONTROL | WRITE_DAC)};
    if (!wslservice)
    {
        THROW_LAST_ERROR_IF(GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST);

        // wslservice doesn't exist, this is expected
        return;
    }

    // Sanity check: Validate that this is indeed an MSIX service
    wil::unique_hkey key = wsl::windows::common::registry::OpenKey(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services", KEY_READ);
    THROW_LAST_ERROR_IF(!key);

    const auto AppUserModelId = wsl::windows::common::registry::ReadString(key.get(), L"WSLService", L"AppUserModelId", L"");
    key.reset();

    DWORD DeleteStatus = ERROR_NOT_SUPPORTED;

    if (!AppUserModelId.empty())
    {
        GrantDeletePermissionToSystem(wslservice.get());
        wslservice.reset(OpenService(manager.get(), L"wslservice", DELETE));

        if (DeleteService(wslservice.get()))
        {
            DeleteStatus = NO_ERROR;
        }
        else
        {
            DeleteStatus = GetLastError();
        }
    }

    WSL_LOG(
        "MsixServiceRegistrationFound",
        TraceLoggingValue(AppUserModelId.c_str(), "AppModelUserId"),
        TraceLoggingValue(DeleteStatus, "DeleteStatus"));
}
CATCH_LOG();

bool RemoveRegistryKeyProtectionImpl(LPCWSTR Path)
{
    if (!SfcIsKeyProtected(HKEY_LOCAL_MACHINE, Path, KEY_WOW64_64KEY))
    {
        return false; // The key doesn't exist or isn't protected, nothing to do.
    }

    // Open the registry key.
    auto key = wsl::windows::common::registry::OpenKey(HKEY_LOCAL_MACHINE, Path, KEY_READ | KEY_WRITE, REG_OPTION_BACKUP_RESTORE);

    // Get its security descriptor.
    DWORD bufferSize = 0;
    auto result = RegGetKeySecurity(key.get(), OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, nullptr, &bufferSize);
    THROW_WIN32_IF(result, result != ERROR_INSUFFICIENT_BUFFER);

    std::vector<char> buffer(bufferSize);

    result = RegGetKeySecurity(key.get(), OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, buffer.data(), &bufferSize);
    THROW_IF_WIN32_ERROR(result);

    // Get the ACL from the security descriptor
    // N.B. 'acl' is stored inside the security descriptor buffer, and so doesn't need to be individually deleted.
    PACL acl{};
    BOOL present{};
    BOOL defaulted{};
    THROW_IF_WIN32_BOOL_FALSE(GetSecurityDescriptorDacl(reinterpret_cast<PSECURITY_DESCRIPTOR>(buffer.data()), &present, &acl, &defaulted));

    // Grant write access to local administrator group.
    // N.B. A registry key is considered protected if:
    // - TrustedInstaller has GENERIC_ALL or KEY_FULL_ACCESS granted
    // - No other ACL grants write access to anyone else
    // - No deny ACL is set for TrustedInstaller
    auto [localAdministratorsSid, sidBuffer] =
        wsl::windows::common::security::CreateSid(SECURITY_NT_AUTHORITY, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);

    EXPLICIT_ACCESS newAce{};
    newAce.grfAccessMode = GRANT_ACCESS;
    newAce.grfAccessPermissions = KEY_WRITE;
    newAce.grfInheritance = NO_INHERITANCE;
    BuildTrusteeWithSid(&newAce.Trustee, localAdministratorsSid);

    // Create an updated ACL.
    wsl::windows::common::security::unique_acl newAcl{};
    THROW_IF_WIN32_ERROR(SetEntriesInAcl(1, &newAce, acl, &newAcl));

    // Create a new security descriptor with the updated ACL.
    SECURITY_DESCRIPTOR newDescriptor{};
    THROW_IF_WIN32_BOOL_FALSE(InitializeSecurityDescriptor(&newDescriptor, SECURITY_DESCRIPTOR_REVISION));
    THROW_IF_WIN32_BOOL_FALSE(SetSecurityDescriptorDacl(&newDescriptor, true, newAcl.get(), false));

    // Update the key security descriptor.
    THROW_IF_WIN32_ERROR_MSG(
        RegSetKeySecurity(key.get(), DACL_SECURITY_INFORMATION, &newDescriptor), "Failed to update key security for key: %ls", Path);

    key.reset();

    if constexpr (wsl::shared::Debug)
    {
        THROW_HR_IF_MSG(
            E_FAIL, SfcIsKeyProtected(HKEY_LOCAL_MACHINE, Path, KEY_WOW64_64KEY), "Failed to remove protection for key: %ls", Path);
    }

    return true;
}

extern "C" UINT __stdcall RemoveRegistryKeyProtections(MSIHANDLE install)
{
    try
    {
        auto restore = wsl::windows::common::security::AcquirePrivileges({SE_BACKUP_NAME, SE_RESTORE_NAME});

        for (const auto* key : {
                 LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\IdListAliasTranslations\WSL)",
                 LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\IdListAliasTranslations\WSLLegacy)",
                 LR"(SOFTWARE\Classes\Directory\Background\shell\WSL)",
                 LR"(SOFTWARE\Classes\Directory\Background\shell\WSL\command)",
                 LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\{B2B4A4D1-2754-4140-A2EB-9A76D9D7CDC6})",
                 LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel)",
             })
        {
            bool updated = false;
            auto result = wil::ResultFromException([&updated, key]() { updated = RemoveRegistryKeyProtectionImpl(key); });

            if (updated || FAILED(result))
            {
                WSL_LOG(
                    "RemoveKeyProtection",
                    TraceLoggingValue(key, "key"),
                    TraceLoggingValue(result, "error"),
                    TraceLoggingValue(updated, "updated"));
            }
        }
    }
    CATCH_LOG();

    return NOERROR;
}

bool CleanExplorerShortcutFlags(LPCWSTR Sid)
{
    constexpr auto valueName = L"Attributes";

    const auto keyPath = std::format(
        LR"({}\Software\Microsoft\Windows\CurrentVersion\Explorer\CLSID\{{B2B4A4D1-2754-4140-A2EB-9A76D9D7CDC6}}\ShellFolder)", Sid);

    auto [key, result] = wsl::windows::common::registry::OpenKeyNoThrow(HKEY_USERS, keyPath.c_str(), KEY_READ | KEY_WRITE);

    if (!SUCCEEDED(result))
    {
        // Either the key doesn't exist, or the user isn't logged in.
        THROW_HR_IF(result, result != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && result != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
        return false;
    }

    auto flags = wsl::windows::common::registry::ReadDword(key.get(), nullptr, valueName, 0);
    if (WI_IsFlagClear(flags, SFGAO_NONENUMERATED))
    {
        // The problematic flag is not set, nothing to do.
        return false;
    }

    WI_ClearFlag(flags, SFGAO_NONENUMERATED);

    wsl::windows::common::registry::WriteDword(key.get(), nullptr, valueName, flags);
    return true;
}

extern "C" UINT __stdcall CleanExplorerState(MSIHANDLE install)
{
    // N.B. This method is imperfect because it can only access the registry hives of logged in users.

    try
    {
        const auto profiles = wsl::windows::common::registry::OpenKey(
            HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList)", KEY_READ);

        // List all available profiles on the machine.
        for (const auto& [name, key] : wsl::windows::common::registry::EnumKeys(profiles.get(), KEY_READ))
        {
            // Look for full profiles.
            if (wsl::windows::common::registry::ReadDword(key.get(), nullptr, L"FullProfile", 0))
            {
                bool changed = false;
                auto result = wil::ResultFromException([&]() { changed = CleanExplorerShortcutFlags(name.c_str()); });

                if (changed || FAILED(result))
                {
                    WSL_LOG(
                        "ClearExplorerFlag",
                        TraceLoggingValue(name.c_str(), "sid"),
                        TraceLoggingValue(result, "error"),
                        TraceLoggingValue(changed, "changed"));
                }
            }
        }
    }
    CATCH_LOG();

    return NOERROR;
}

extern "C" UINT __stdcall CleanMsixState(MSIHANDLE install)
{
    try
    {
        WSL_LOG("CleanMsixState");

        const std::map<LPCWSTR, LPCWSTR> keys{
            {L"SYSTEM\\CurrentControlSet\\Services\\EventLog\\Application", L"WSL"},
            {L"SOFTWARE\\Classes\\CLSID", L"{7e6ad219-d1b3-42d5-b8ee-d96324e64ff6}"},
            {L"SOFTWARE\\Classes\\AppID", L"{17696EAC-9568-4CF5-BB8C-82515AAD6C09}"},
            {L"SOFTWARE\\Microsoft\\Terminal Server Client", L"Default"},
            {L"SOFTWARE\\Microsoft\\Terminal Server Client\\Default", L"OptionalAddIns"},
            {L"SOFTWARE\\Microsoft\\Terminal Server Client\\Default\\OptionalAddIns", L"WSLDVC_PACKAGE"}};

        for (const auto& e : keys)
        {
            try
            {
                DeleteRegistryKeyIfVolatile(e.first, e.second);
            }
            catch (...)
            {
                LOG_CAUGHT_EXCEPTION_MSG("Failed to clear registry key: %ls/%ls", e.first, e.second);
            }
        }

        /*
         * Because of a probable bug in MSIX / Packaged COM, it's possible that an old registration is still present on the machine,
         * which will break instantiations of LxssUserSessions.
         * Because this method executes after all MSIX packages have been removed, we know that this registration shouldn't be there,
         * so delete it if it still happens to be there.
         * See: https://github.com/microsoft/WSL/issues/10782
         */

        try
        {

            const auto packagedComClassIndex{
                wsl::windows::common::registry::OpenKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\PackagedCom\\ClassIndex", KEY_WRITE)};

            if (wsl::windows::common::registry::DeleteKey(packagedComClassIndex.get(), L"{A9B7A1B9-0671-405C-95F1-E0612CB4CE7E}"))
            {
                WSL_LOG("OldComRegistrationCleared");
            }
        }
        CATCH_LOG();

        /*
         * Because of another probable MSIX bug, wslservice can sometimes be left even after an WSL < 2.0 package is removed, which causes the installation to fail.
         * If found, we delete the service registration.
         * See: https://github.com/microsoft/WSL/issues/10831
         */

        RemoveMsixService();
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
    }

    // Always succeed here since failure in this method aren't fatal.
    return NOERROR;
}

extern "C" UINT __stdcall DeprovisionMsix(MSIHANDLE install)
try
{
    WSL_LOG("DeprovisionMsix");
    WriteInstallLog("MSI install: DeprovisionMsix");

    const winrt::Windows::Management::Deployment::PackageManager packageManager;
    const auto result = packageManager.DeprovisionPackageForAllUsersAsync(wsl::windows::common::wslutil::c_msixPackageFamilyName).get();
    LOG_IF_FAILED_MSG(result.ExtendedErrorCode(), "%ls", result.ErrorText().c_str());

    return NOERROR;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();

    IGNORE_MSIX_ERROR_IF_DIRECT_MSI_EXECUTION_SUPPORTED();

    const auto error = wsl::windows::common::wslutil::GetErrorString(wil::ResultFromCaughtException());
    DisplayError(install, wsl::shared::Localization::MessagedFailedToRemoveMsix(error).c_str());

    return ERROR_INSTALL_FAILURE;
}

extern "C" UINT __stdcall RemoveMsixAsSystem(MSIHANDLE install)
try
{
    WSL_LOG("RemoveMsixAsSystem");
    WriteInstallLog("MSI install: RemoveMsixAsSystem");

    const winrt::Windows::Management::Deployment::PackageManager packageManager;

    for (const auto& e : packageManager.FindPackages(wsl::windows::common::wslutil::c_msixPackageFamilyName))
    {
        WSL_LOG("RemovePackage", TraceLoggingValue(e.Id().FullName().c_str(), "FullName"));

        ThrowIfOperationError(packageManager.RemovePackageAsync(
            e.Id().FullName(), winrt::Windows::Management::Deployment::RemovalOptions::RemoveForAllUsers));
    }

    return NOERROR;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();

    IGNORE_MSIX_ERROR_IF_DIRECT_MSI_EXECUTION_SUPPORTED();

    const auto error = wsl::windows::common::wslutil::GetErrorString(wil::ResultFromCaughtException());
    DisplayError(install, wsl::shared::Localization::MessagedFailedToRemoveMsix(error).c_str());

    return ERROR_INSTALL_FAILURE;
}

extern "C" UINT __stdcall RemoveMsixAsUser(MSIHANDLE install)
try
{
    WSL_LOG("RemoveMsixAsUser");
    WriteInstallLog("MSI install: RemoveMsixAsUser");

    const winrt::Windows::Management::Deployment::PackageManager packageManager;

    for (const auto& e : packageManager.FindPackagesForUser(L"", wsl::windows::common::wslutil::c_msixPackageFamilyName))
    {
        WSL_LOG("RemovePackage", TraceLoggingValue(e.Id().FullName().c_str(), "FullName"));

        ThrowIfOperationError(packageManager.RemovePackageAsync(e.Id().FullName()));
    }

    return NOERROR;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();

    IGNORE_MSIX_ERROR_IF_DIRECT_MSI_EXECUTION_SUPPORTED();

    const auto error = wsl::windows::common::wslutil::GetErrorString(wil::ResultFromCaughtException());
    DisplayError(install, wsl::shared::Localization::MessagedFailedToRemoveMsix(error).c_str());

    return ERROR_INSTALL_FAILURE;
}

wsl::windows::common::filesystem::TempFile ExtractMsix(MSIHANDLE install)
{
    // N.B. We need to open the database this way instead of calling MsiGetActiveDatabase() because
    // this is deferred action so we don't have access to the MSI context here.
    // The MSIX needs to be extracted like this because in the case of an upgrade this action runs before 'MoveFiles' so the WSL directory isn't available yet.

    const auto installTarget = GetInstallTarget(install);

    unique_msi_handle database;
    THROW_IF_WIN32_ERROR_MSG(
        MsiOpenDatabase(installTarget.c_str(), MSIDBOPEN_READONLY, &database), "Failed to open database: %ls", installTarget.c_str());

    THROW_LAST_ERROR_IF(!database);

    unique_msi_handle view;
    THROW_IF_WIN32_ERROR(MsiDatabaseOpenView(database.get(), L"SELECT Data,Name FROM Binary WHERE Name='msixpackage'", &view));

    THROW_IF_WIN32_ERROR(MsiViewExecute(view.get(), NULL));

    unique_msi_handle record;
    THROW_IF_WIN32_ERROR(MsiViewFetch(view.get(), &record));

    auto file = wsl::windows::common::filesystem::TempFile(
        GENERIC_WRITE, 0, CREATE_ALWAYS, wsl::windows::common::filesystem::TempFileFlags::None, L"msix");

    std::vector<char> buffer(1024 * 1024);
    while (true)
    {
        DWORD size = static_cast<DWORD>(buffer.size());
        THROW_IF_WIN32_ERROR(MsiRecordReadStream(record.get(), 1, buffer.data(), &size));
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.Handle.get(), buffer.data(), size, nullptr, nullptr));

        if (size < buffer.size())
        {
            break;
        }
    }

    return file;
}

extern "C" UINT __stdcall InstallMsixAsUser(MSIHANDLE install)
try
{
    WSL_LOG("InstallMsixAsUser");
    WriteInstallLog("MSI install: InstallMsixAsUser");

    // RegisterPackageByFamilyNameAsync() cannot be run as SYSTEM.
    //  If this thread runs as SYSTEM, simply skip this step.
    if (wsl::windows::common::security::IsTokenLocalSystem(nullptr))
    {
        WSL_LOG("InstallMsixAsUserSkipped");
        return NOERROR;
    }

    const winrt::Windows::Management::Deployment::PackageManager packageManager;
    ThrowIfOperationError(packageManager.RegisterPackageByFamilyNameAsync(
        wsl::windows::common::wslutil::c_msixPackageFamilyName,
        nullptr,
        winrt::Windows::Management::Deployment::DeploymentOptions::ForceTargetApplicationShutdown |
            winrt::Windows::Management::Deployment::DeploymentOptions::ForceApplicationShutdown,
        nullptr,
        nullptr));

    return NOERROR;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();

    IGNORE_MSIX_ERROR_IF_DIRECT_MSI_EXECUTION_SUPPORTED();

    const auto errorCode = wil::ResultFromCaughtException();

    const auto error = wsl::windows::common::wslutil::GetErrorString(errorCode);
    DisplayError(install, wsl::shared::Localization::MessagedFailedToInstallMsix(error).c_str());

    return ERROR_INSTALL_FAILURE;
}

extern "C" UINT __stdcall InstallMsix(MSIHANDLE install)
try
{
    auto msixFile = ExtractMsix(install);

    // Release a file handle to the MSIX file so that it can be installed.
    msixFile.Handle.reset();

    WSL_LOG("InstallMsix", TraceLoggingValue(msixFile.Path.c_str(), "Path"));
    WriteInstallLog("MSI install: InstallMsix");

    winrt::Windows::Management::Deployment::PackageManager packageManager;

    winrt::Windows::Foundation::Uri uri(msixFile.Path.c_str());
    winrt::Windows::Management::Deployment::StagePackageOptions options;
    options.ForceUpdateFromAnyVersion(true);

    try
    {
        try
        {
            ThrowIfOperationError(packageManager.StagePackageByUriAsync(uri, options));
        }
        catch (...)
        {
            // For convenience, automatically trust the MSIX's certificate if this is NOT an official build and
            // the package installation failed because of an untrusted certificate.
#ifndef WSL_OFFICIAL_BUILD
            auto error = wil::ResultFromCaughtException();
            if (error == CERT_E_UNTRUSTEDROOT)
            {
                TrustPackageCertificate(msixFile.Path.c_str());
                ThrowIfOperationError(packageManager.StagePackageByUriAsync(uri, options));
            }
#else
            throw;
#endif
        }

        ThrowIfOperationError(packageManager.ProvisionPackageForAllUsersAsync(wsl::windows::common::wslutil::c_msixPackageFamilyName));
    }
    catch (...)
    {
        // On Windows Server, ProvisionPackageForAllUsersAsync() fails with ERROR_NOT_SUPPORTED or ERROR_INSTALL_FAILED.
        // Using powershell as a fallback in case we hit this issue.
        auto error = wil::ResultFromCaughtException();
        if ((error == REGDB_E_CLASSNOTREG || error == HRESULT_FROM_WIN32(ERROR_INSTALL_REGISTRATION_FAILURE) ||
             error == HRESULT_FROM_WIN32(ERROR_INSTALL_PACKAGE_NOT_FOUND)) &&
            IsWindowsServerCoreWithMsiSupport())
        {
            // MSIX applications are not supported on ServerCore SKU's so as long as this build has direct MSI support
            // the installation can continue.
            return NOERROR;
        }
        else if ((error == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) || error == HRESULT_FROM_WIN32(ERROR_INSTALL_FAILED)) && IsWindowsServer())
        {
            std::wstring commandLine;
            wil::GetSystemDirectoryW(commandLine);

            // N.B. powershell is always installed under 'v1.0' so this path is constant.
            commandLine +=
                L"\\WindowsPowerShell\\v1.0\\powershell.exe -ExecutionPolicy Bypass -NoProfile -NonInteractive -Command "
                L"Add-AppxProvisionedPackage "
                L"-Online -PackagePath \"" +
                msixFile.Path.wstring() + L"\" -SkipLicense";

            WSL_LOG("CallPS", TraceLoggingValue(commandLine.c_str(), "CommandLine"));

            wsl::windows::common::SubProcess process(nullptr, commandLine.c_str());
            process.SetFlags(CREATE_NO_WINDOW);
            process.SetShowWindow(SW_HIDE);

            auto output = process.RunAndCaptureOutput();
            if (output.ExitCode != 0)
            {
                if (output.Stderr.size() > 250) // Limit how big the error message can get
                {
                    output.Stderr.resize(250);
                }

                DisplayError(install, wsl::shared::Localization::MessagedFailedToInstallMsix(output.Stderr).c_str());

                return ERROR_INSTALL_FAILURE;
            }
        }
        else
        {
            THROW_IF_FAILED(error);
        }
    }

    WSL_LOG("InstallMsixComplete");

    return NOERROR;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();

    IGNORE_MSIX_ERROR_IF_DIRECT_MSI_EXECUTION_SUPPORTED();

    auto error = wsl::windows::common::wslutil::GetErrorString(wil::ResultFromCaughtException());
    DisplayError(install, wsl::shared::Localization::MessagedFailedToInstallMsix(error).c_str());

    return ERROR_INSTALL_FAILURE;
}

extern "C" UINT __stdcall WslFinalizeInstallation(MSIHANDLE install)
{
    try
    {
        WSL_LOG("WslFinalizeInstallation");
        WriteInstallLog(std::format("MSI install: WslFinalizeInstallation"));
    }
    CATCH_LOG();

    return NOERROR;
}

extern "C" UINT __stdcall WslValidateInstallation(MSIHANDLE install)
try
{
    WSL_LOG("WslValidateInstallation");

    WriteInstallLog(std::format("MSI install: WslValidateInstallation"));

    // TODO: Use a more precise version check so we don't install if the Windows build doesn't support lifted.

    if (wsl::windows::common::helpers::GetWindowsVersion().BuildNumber < wsl::windows::common::helpers::Vibranium)
    {
        DisplayError(install, wsl::windows::common::wslutil::GetErrorString(WSL_E_OS_NOT_SUPPORTED).c_str());
        return ERROR_INSTALL_FAILURE;
    }

    return NOERROR;
}
catch (...)
{
    LOG_CAUGHT_EXCEPTION();

    return ERROR_INSTALL_FAILURE;
}

void RegisterLspCategoriesImpl(DWORD flags)
{
    const auto installRoot = wsl::windows::common::wslutil::GetMsiPackagePath();
    THROW_HR_IF(E_INVALIDARG, !installRoot.has_value());

    for (const auto& e : {L"wsl.exe", L"wslhost.exe", L"wslrelay.exe", L"wslg.exe", L"wslservice.exe"})
    {
        auto executable = installRoot.value() + e;
        INT error{};

        DWORD previous{};
        LOG_HR_IF_MSG(
            E_UNEXPECTED,
            WSCSetApplicationCategory(executable.c_str(), static_cast<DWORD>(executable.size()), nullptr, 0, flags, &previous, &error) == SOCKET_ERROR,
            "Failed to register LSP category for : %ls, flags: %lu, error: %i",
            executable.c_str(),
            flags,
            error);
    }
}

extern "C" UINT __stdcall RegisterLspCategories(MSIHANDLE install)
{
    /*
     * This logic is required because some VPN providers register LSP components that break WSL.
     * See: https://github.com/microsoft/WSL/issues/4177/
     */

    try
    {
        WSL_LOG("RegisterLspCategories");
        RegisterLspCategoriesImpl(LSP_SYSTEM);
    }
    CATCH_LOG();

    // Failures in this method aren't fatal.
    return NOERROR;
}

extern "C" UINT __stdcall UnregisterLspCategories(MSIHANDLE install)
{
    try
    {
        WSL_LOG("UnregisterLspCategories");
        RegisterLspCategoriesImpl(0); // '0' means removing the entry.
    }
    CATCH_LOG();

    // Failures in this method aren't fatal.
    return NOERROR;
}

std::wstring GetWslSettingsInstalledExePath(MSIHANDLE install)
{
    const auto wslSettingsInstallFolder = GetMsiProperty(install, c_wslSettingsInstalledDirectoryPropertyName);
    THROW_HR_IF_MSG(E_UNEXPECTED, wslSettingsInstallFolder.empty(), "GetMsiProperty for '%s' resulted in unexpected empty string", c_wslSettingsInstalledDirectoryPropertyName);

    auto wslSettingsInstalledExePath = std::filesystem::path(wslSettingsInstallFolder) / L"wslsettings.exe";
    return wslSettingsInstalledExePath.make_preferred().wstring();
}

// The following function is borrowed directly from the Windows App SDK
std::wstring ComputeAppId(const std::wstring& seed)
{
    // Prefix = App -- Simple human readable piece to help organize these together.
    // AppId = Prefix + Hash(seed)

    const std::hash<std::wstring> hasher;
    const auto hash = hasher(seed);
    uint64_t hash64 = static_cast<uint64_t>(hash);

    // Simulate a larger hash on 32bit platforms to keep the id length consistent.
    if constexpr (sizeof(size_t) < sizeof(uint64_t))
    {
        hash64 = (static_cast<uint64_t>(hash) << 32) | static_cast<uint64_t>(hash);
    }

    wchar_t hashString[17]{}; // 16 + 1 characters for 64bit value represented as a string with a null terminator.
    THROW_IF_FAILED(StringCchPrintf(hashString, _countof(hashString), L"%I64x", hash64));

    std::wstring result{c_progIdPrefix};
    result += hashString;
    return result;
}

std::wstring ComputeProgId(const std::wstring& appId)
{
    return std::wstring(appId + c_protocolProgIdSuffix);
}

extern "C" UINT __stdcall CalculateWslSettingsProtocolIds(MSIHANDLE install)
{
    try
    {
        WSL_LOG("CalculateWslSettingsProtocolIds");

        const auto wslSettingsInstalledExePath = GetWslSettingsInstalledExePath(install);
        THROW_HR_IF_MSG(
            E_UNEXPECTED,
            wslSettingsInstalledExePath.empty(),
            "Fetching WSL Settings installed exe path resulted in unexpected empty string");

        const auto appId = ComputeAppId(wslSettingsInstalledExePath);
        const auto progId = ComputeProgId(appId);

        UINT result = MsiSetProperty(install, c_wslSettingsAppIDPropertyName, appId.c_str());
        THROW_HR_IF_MSG(E_UNEXPECTED, result != ERROR_SUCCESS, "MsiSetProperty for '%s' failed with %u", c_wslSettingsAppIDPropertyName, result);

        result = MsiSetProperty(install, c_wslSettingsProgIDPropertyName, progId.c_str());
        THROW_HR_IF_MSG(E_UNEXPECTED, result != ERROR_SUCCESS, "MsiSetProperty for '%s' failed with %u", c_wslSettingsProgIDPropertyName, result);
    }
    CATCH_LOG();

    // Failures in this method aren't fatal.

    return NOERROR;
}

EXTERN_C BOOL STDAPICALLTYPE DllMain(_In_ HINSTANCE Instance, _In_ DWORD Reason, _In_opt_ LPVOID Reserved)
{
    wil::DLLMain(Instance, Reason, Reserved);

    switch (Reason)
    {
    case DLL_PROCESS_ATTACH:
        WslTraceLoggingInitialize(LxssTelemetryProvider, false);
        break;

    case DLL_PROCESS_DETACH:
        WslTraceLoggingUninitialize();
        break;
    }

    return TRUE;
}