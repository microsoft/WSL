/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    InstallerTests.cpp

Abstract:

    This file contains test cases for the installation process.

--*/

#include "precomp.h"
#include <Sfc.h>

#include "Common.h"
#include "registry.hpp"
#include "PluginTests.h"

using namespace wsl::windows::common::registry;

extern std::wstring g_dumpFolder;
static std::wstring g_pipelineBuildId;

class InstallerTests
{
    std::wstring m_msixPackagePath;
    std::wstring m_msiPath;
    std::wstring m_msixInstalledPath;
    std::filesystem::path m_installedPath;
    bool m_initialized = false;
    winrt::Windows::Management::Deployment::PackageManager m_packageManager;
    wil::unique_hkey m_lxssKey = wsl::windows::common::registry::OpenLxssMachineKey(KEY_ALL_ACCESS);
    wil::unique_schandle m_scm{OpenSCManagerW(nullptr, SERVICES_ACTIVE_DATABASE, GENERIC_READ | GENERIC_EXECUTE)};

    wil::unique_hfile nulDevice{CreateFileW(
        L"nul", GENERIC_READ, (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};

    WSL_TEST_CLASS(InstallerTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        VERIFY_ARE_EQUAL(LxsstuInitialize(FALSE), TRUE);
        m_initialized = true;

        WEX::Common::String MsixPackagePath;
        WEX::TestExecution::RuntimeParameters::TryGetValue(L"Package", MsixPackagePath);
        m_msixPackagePath = std::filesystem::weakly_canonical(static_cast<std::wstring>(MsixPackagePath)).wstring();
        VERIFY_IS_FALSE(m_msixPackagePath.empty());

        for (const auto& e : m_packageManager.FindPackages(wsl::windows::common::wslutil::c_msixPackageFamilyName))
        {
            VERIFY_IS_TRUE(m_msixInstalledPath.empty());
            m_msixInstalledPath = std::wstring(e.InstalledLocation().Path().c_str());
        }

#ifdef WSL_DEV_THIN_MSI_PACKAGE

        m_msiPath = std::filesystem::weakly_canonical(WSL_DEV_THIN_MSI_PACKAGE).wstring();

#else

        VERIFY_IS_TRUE(IsInstallerMsixInstalled(), L"Installer MSIX absent, can't run the tests");
        m_msiPath = (std::filesystem::temp_directory_path() / L"wsl.msi").wstring();
        VERIFY_IS_TRUE(std::filesystem::copy_file(m_msixInstalledPath + L"\\wsl.msi", m_msiPath, std::filesystem::copy_options::overwrite_existing));
#endif

        auto installPath = wsl::windows::common::wslutil::GetMsiPackagePath();
        VERIFY_IS_TRUE(installPath.has_value());

        m_installedPath = std::move(installPath.value());

        wsl::windows::common::helpers::SetHandleInheritable(nulDevice.get());

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {

#ifndef WSL_DEV_THIN_MSI_PACKAGE

        if (!m_msiPath.empty())
        {
            std::filesystem::remove(m_msiPath);
        }

#endif

        if (m_initialized)
        {
            LxsstuUninitialize(FALSE);
        }

        return true;
    }

    DWORD GetWslInstallerServiceState() const
    {
        const wil::unique_schandle service{OpenServiceW(m_scm.get(), L"Wslinstaller", SERVICE_QUERY_STATUS)};
        VERIFY_IS_NOT_NULL(service);

        SERVICE_STATUS status{};
        VERIFY_IS_TRUE(QueryServiceStatus(service.get(), &status));

        return status.dwCurrentState;
    }

    void WaitForInstallerServiceStop()
    {
        DWORD lastState = -1;
        auto pred = [&]() {
            lastState = GetWslInstallerServiceState();
            THROW_HR_IF(E_FAIL, lastState != SERVICE_STOPPED);
        };

        try
        {
            wsl::shared::retry::RetryWithTimeout<void>(pred, std::chrono::hours(3), std::chrono::minutes(2));
        }
        catch (...)
        {
            LogError("InstallerService state: %lu", lastState);
            VERIFY_FAIL("Timed out waiting for the installer service to stop");
        }
    }

    static std::wstring GenerateMsiLogPath()
    {
        // Note: canonical is required because msiexec seems to be unable to deal with symlinks.
        static int counter = 0;
        return std::format(L"{}\\msi-install-{}.txt", std::filesystem::canonical(g_dumpFolder).c_str(), counter++);
    }

    bool IsInstallerMsixInstalled() const
    {
        return std::filesystem::exists(m_msixInstalledPath + L"\\wslinstaller.exe");
    }

    bool IsMsixInstalled() const
    {
        return m_packageManager.FindPackagesForUser(L"", wsl::windows::common::wslutil::c_msixPackageFamilyName).First().HasCurrent();
    }

    static void CallMsiExec(const std::wstring& Args)
    {
        std::wstring commandLine;
        THROW_IF_FAILED(wil::GetSystemDirectoryW(commandLine));
        commandLine += L"\\msiexec.exe " + Args;

        LogInfo("Calling msiexec: %ls", commandLine.c_str());

        // There is a potential race condition given that we have no easy way to know when the msiexec process
        // created by the installer service will release the MSI mutex.
        // If the mutex is still held when we call msiexec, it will fails with ERROR_INSTALL_ALREADY_RUNNING
        // so retry for up to two minutes if we get that error back.

        DWORD exitCode = -1;
        wsl::shared::retry::RetryWithTimeout<void>(
            [&]() {
                exitCode = LxsstuRunCommand(commandLine.data());
                THROW_HR_IF(E_ABORT, exitCode == ERROR_INSTALL_ALREADY_RUNNING);
            },
            std::chrono::seconds(1),
            std::chrono::minutes(2),
            []() { return wil::ResultFromCaughtException() == E_ABORT; });

        VERIFY_ARE_EQUAL(0L, exitCode);
    }

    std::wstring GetMsiProductCode() const
    {
        return wsl::windows::common::registry::ReadString(m_lxssKey.get(), L"MSI", L"ProductCode", L"");
    }

    void UninstallMsi()
    {
        auto productCode = GetMsiProductCode();
        VERIFY_IS_FALSE(productCode.empty());

        CallMsiExec(std::format(L"/qn /norestart /x {} /L*V {}", productCode, GenerateMsiLogPath()));
    }

    void InstallMsi()
    {
        CallMsiExec(std::format(L"/qn /norestart /i {} /L*V {}", m_msiPath, GenerateMsiLogPath()));
    }

    void InstallMsix() const
    {
        const auto result =
            m_packageManager
                .AddPackageAsync(winrt::Windows::Foundation::Uri{m_msixPackagePath}, nullptr, winrt::Windows::Management::Deployment::DeploymentOptions::None)
                .get();

        VERIFY_ARE_EQUAL(result.ExtendedErrorCode(), S_OK);
    }

    void UninstallMsix() const
    {
        auto result = m_packageManager.DeprovisionPackageForAllUsersAsync(wsl::windows::common::wslutil::c_msixPackageFamilyName).get();
        auto errorCode = result.ExtendedErrorCode();
        if (FAILED(errorCode) && errorCode != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            LogError("Error deprovisioning the package: 0x%x, %ls", errorCode, result.ErrorText().c_str());
            VERIFY_FAIL();
        }

        for (const auto& e : m_packageManager.FindPackages(wsl::windows::common::wslutil::c_msixPackageFamilyName))
        {
            // Remove the package for the current user first.
            result = m_packageManager.RemovePackageAsync(e.Id().FullName()).get();
            errorCode = result.ExtendedErrorCode();
            if (FAILED(errorCode) && errorCode != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
            {
                LogError("Error deprovisioning the package: 0x%x, %ls", errorCode, result.ErrorText().c_str());
                VERIFY_FAIL();
            }

            // then remove the package for all users.
            result = m_packageManager
                         .RemovePackageAsync(e.Id().FullName(), winrt::Windows::Management::Deployment::RemovalOptions::RemoveForAllUsers)
                         .get();
            errorCode = result.ExtendedErrorCode();
            if (FAILED(errorCode) && errorCode != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
            {
                LogError("Error deprovisioning the package: 0x%x, %ls", errorCode, result.ErrorText().c_str());
                VERIFY_FAIL();
            }
        }
    }

    bool IsMsiPackageInstalled()
    {
        // Check for wslservice to be installed because MSI installs registry keys before installing services
        return !GetMsiProductCode().empty() && wsl::windows::common::helpers::IsServicePresent(L"wslservice");
    }

    static bool IsMsixInstallerInstalled()
    {
        return wsl::windows::common::helpers::IsServicePresent(L"wslinstaller");
    }

    void WaitForMsiPackageInstall()
    {
        auto pred = [this]() { THROW_HR_IF(E_FAIL, !IsMsiPackageInstalled()); };

        try
        {
            // It is possible for the 'DeprovisionMsix' stage of the MSI installation to take a long time.
            // On vb_release, up to 7 minutes have been observed. Wait for up to 20 minutes to be safe.
            wsl::shared::retry::RetryWithTimeout<void>(pred, std::chrono::seconds(1), std::chrono::minutes(20));
        }
        catch (...)
        {
            VERIFY_FAIL("Timed out waiting for MSI package installation");
        }
    }

    static void ValidateInstalledVersion(LPCWSTR expectedVersion)
    {
        auto pred = [expectedVersion]() {
            // Validate that we're not using inbox WSL
            auto [output, _] = LxsstuLaunchWslAndCaptureOutput(L"--version");
            if (output.find(expectedVersion) == std::string::npos)
            {
                LogInfo("Package is not installed yet. Output: %ls", output.c_str());
                THROW_HR(E_FAIL);
            }

            LogInfo("Package is properly installed. Output: %ls", output.c_str());
        };

        // Sadly the provisioning of MSIX packages for user accounts isn't synchronous so we need to wait until the package
        // becomes visible.
        try
        {
            wsl::shared::retry::RetryWithTimeout<void>(pred, std::chrono::seconds(1), std::chrono::minutes(2));
        }
        catch (...)
        {
            VERIFY_FAIL("Timed out waiting for MSIX package to be available");
        }
    }

    void ValidatePackageInstalledProperly()
    {
        ValidateInstalledVersion(WIDEN(WSL_PACKAGE_VERSION));

        auto checkInstalled = []() {
            auto commandLine = LxssGenerateWslCommandLine(L"echo OK");
            auto [output, _, exitCode] = LxsstuLaunchCommandAndCaptureOutputWithResult(commandLine.data());
            if (exitCode != 0 && output.find(L"Wsl/ERROR_SHARING_VIOLATION") != std::wstring::npos)
            {
                THROW_HR(HRESULT_FROM_WIN32(ERROR_RETRY));
            }

            return std::make_pair(exitCode, output);
        };

        // When upgrading from MSIX, wsl.exe might not be available right away. Retry if we get Wsl/ERROR_SHARING_VIOLATION back.
        std::wstring output;
        int exitCode = -1;
        try
        {
            std::tie(exitCode, output) = wsl::shared::retry::RetryWithTimeout<std::pair<int, std::wstring>>(
                checkInstalled, std::chrono::seconds(1), std::chrono::minutes(2), []() {
                    return wil::ResultFromCaughtException() == HRESULT_FROM_WIN32(ERROR_RETRY);
                });
        }
        catch (...)
        {
            VERIFY_FAIL("Timed out waiting for WSL to be installed.");
        }

        VERIFY_ARE_EQUAL(exitCode, 0);
        VERIFY_ARE_EQUAL(output, L"OK\n");

        // Check that the installed version is the one we expect
        const auto key = wsl::windows::common::registry::OpenLxssMachineKey();
        const auto version = wsl::windows::common::registry::ReadString(key.get(), L"MSI", L"Version", L"");

        VERIFY_ARE_EQUAL(version, WIDEN(WSL_PACKAGE_VERSION));
        VERIFY_IS_FALSE(GetMsiProductCode().empty());

        // TODO: check wslsupport, wslapi and p9rdr
    }

    void DeleteProductCode() const
    {
        const auto msiKey = wsl::windows::common::registry::OpenKey(m_lxssKey.get(), L"MSI", KEY_ALL_ACCESS);
        wsl::windows::common::registry::DeleteKeyValue(msiKey.get(), L"ProductCode");
    }

    void InstallGitHubRelease(const std::wstring& version)
    {
        auto arch = wsl::shared::Arm64 ? L".0.arm64" : L".0.x64";

        std::wstring installerFile;
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&installerFile]() { DeleteFile(installerFile.c_str()); });

        if (auto found = L"wsl." + version + arch + L".msi"; PathFileExists(found.c_str()))
        {
            installerFile = std::filesystem::weakly_canonical(found);
            cleanup.release();
        }
        else if (auto found = L"Microsoft.WSL_" + version + L".0_x64_ARM64.msixbundle"; PathFileExists(found.c_str()))
        {
            installerFile = std::filesystem::weakly_canonical(found);
            cleanup.release();
        }
        else
        {
            LogInfo("Downloading: %ls", version.c_str());

            VERIFY_IS_TRUE(g_pipelineBuildId.empty()); // Pipeline builds should have the installers already available
            auto release = wsl::windows::common::wslutil::GetGitHubReleaseByTag(version);
            auto asset = wsl::windows::common::wslutil::GetGitHubAssetFromRelease(release);
            VERIFY_IS_TRUE(asset.has_value());

            auto downloadPath = wsl::windows::common::wslutil::DownloadFile(asset->second.url, asset->second.name);
            installerFile = std::move(downloadPath);
        }

        LogInfo("Installing: %ls", installerFile.c_str());
        if (wsl::shared::string::EndsWith<wchar_t>(installerFile, L".msi"))
        {
            CallMsiExec(std::format(L"/qn /norestart /i {} /L*V {}", installerFile, GenerateMsiLogPath()));
        }
        else
        {
            auto result =
                m_packageManager
                    .AddPackageAsync(winrt::Windows::Foundation::Uri{installerFile}, nullptr, winrt::Windows::Management::Deployment::DeploymentOptions::None)
                    .get();

            VERIFY_SUCCEEDED(result.ExtendedErrorCode());
        }

        ValidateInstalledVersion(version.c_str());
    }

    TEST_METHOD(UpgradeFromWsl130)
    {
        UninstallMsi();
        InstallGitHubRelease(L"1.3.17");

        // Note: we can't use wsl --update here because GitHubUrlOverride was introduced in 2.0.0
        InstallMsi();
        ValidatePackageInstalledProperly();
    }

    TEST_METHOD(UpgradeFromWsl200)
    {
        UninstallMsi();

        // Note: we can't use wsl --update here because wsl 2.0.0 passes REINSTALL=ALL to msiexec
        InstallGitHubRelease(L"2.0.0");
        InstallMsi();
        ValidatePackageInstalledProperly();
    }

    TEST_METHOD(UpgradeFromWsl202)
    {
        UninstallMsi();
        InstallGitHubRelease(L"2.0.2");
        CallWslUpdateViaMsi();
    }

    TEST_METHOD(MsrdcPluginKey)
    {
        // Remove the MSI package.
        UninstallMsi();

        // Create a volatile registry key to emulate what the old MSIX would do.
        const auto key = wsl::windows::common::registry::CreateKey(
            HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Terminal Server Client\\Default", KEY_ALL_ACCESS);
        VERIFY_IS_TRUE(!!key);

        wsl::windows::common::registry::DeleteKey(key.get(), L"OptionalAddIns\\WSLDVC_PACKAGE");

        const auto volatileKey =
            wsl::windows::common::registry::CreateKey(key.get(), L"OptionalAddIns\\WSLDVC_PACKAGE", KEY_READ, nullptr, REG_OPTION_VOLATILE);
        VERIFY_IS_TRUE(wsl::windows::common::registry::IsKeyVolatile(volatileKey.get()));

        // Install the MSI.
        InstallMsi();

        // Validate that the key is correctly written to and isn't volatile anymore.
        auto pluginPath = wsl::windows::common::wslutil::GetMsiPackagePath().value_or(L"");
        VERIFY_IS_FALSE(pluginPath.empty());

        pluginPath += L"WSLDVCPlugin.dll";

        const auto pluginKey = wsl::windows::common::registry::OpenKey(
            HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Terminal Server Client\\Default\\OptionalAddIns\\WSLDVC_PACKAGE", KEY_READ);
        const auto value = wsl::windows::common::registry::ReadString(pluginKey.get(), nullptr, L"Name", L"");
        VERIFY_ARE_EQUAL(value, pluginPath);

        VERIFY_IS_FALSE(wsl::windows::common::registry::IsKeyVolatile(pluginKey.get()));
    }

    TEST_METHOD(UninstallingMsiRemovesInstallerMsix)
    {
        UninstallMsi();
        VERIFY_IS_FALSE(IsMsiPackageInstalled());
        VERIFY_IS_FALSE(IsMsixInstalled());

        InstallMsix();
        WaitForMsiPackageInstall();
        VERIFY_IS_TRUE(IsMsiPackageInstalled());
        VERIFY_IS_TRUE(IsMsixInstalled());
        VERIFY_IS_TRUE(IsMsixInstallerInstalled());

        ValidatePackageInstalledProperly();
    }

    TEST_METHOD(InstallingMsiInstallsGluePackage)
    {
        // Remove the MSI package
        UninstallMsi();
        VERIFY_IS_FALSE(IsMsiPackageInstalled());
        VERIFY_IS_FALSE(IsMsixInstalled());

        // Installing it again and validate that the glue package was added
        InstallMsi();
        VERIFY_IS_TRUE(IsMsiPackageInstalled());
        VERIFY_IS_TRUE(IsMsixInstalled());
        VERIFY_IS_FALSE(IsMsixInstallerInstalled());
        ValidatePackageInstalledProperly();

        // Validate that removing it removes the glue package
        UninstallMsi();
        VERIFY_IS_FALSE(IsMsiPackageInstalled());
        VERIFY_IS_FALSE(IsMsixInstalled());

        // Validate that installing the installer gets the MSI installed properly again
        InstallMsix();
        WaitForMsiPackageInstall();
        VERIFY_IS_TRUE(IsMsiPackageInstalled());
        VERIFY_IS_TRUE(IsMsixInstalled());
        VERIFY_IS_TRUE(IsMsixInstallerInstalled());
        ValidatePackageInstalledProperly();
    }

    TEST_METHOD(UpgradeInstallsTheMsiPackage)
    {
        // Remove the MSIX package
        UninstallMsix();
        VERIFY_IS_FALSE(IsMsixInstalled());

        // Validate that upgrading the MSI installs the MSIX again
        InstallMsi();
        VERIFY_IS_TRUE(IsMsiPackageInstalled());
        VERIFY_IS_TRUE(IsMsixInstalled());
        VERIFY_IS_FALSE(IsMsixInstallerInstalled());
        ValidatePackageInstalledProperly();
    }

    TEST_METHOD(MsixInstallerUpgrades)
    {
        // Remove the MSIX package
        UninstallMsix();
        VERIFY_IS_FALSE(IsMsixInstalled());

        // Remove the MSI package
        UninstallMsi();
        VERIFY_IS_FALSE(IsMsiPackageInstalled());
        VERIFY_IS_FALSE(IsMsixInstalled());

        RegistryKeyChange<std::wstring> change(
            HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss\\MSI", L"Version", L"1.0.0");

        DeleteProductCode();
        VERIFY_IS_TRUE(GetMsiProductCode().empty());

        // Validate that upgrading the MSIX upgrades the MSI
        InstallMsix();
        WaitForMsiPackageInstall();
        VERIFY_IS_TRUE(IsMsiPackageInstalled());
        VERIFY_IS_TRUE(IsMsixInstalled());
        VERIFY_IS_TRUE(IsMsixInstallerInstalled());

        // Validate that the version got upgraded
        const auto key = wsl::windows::common::registry::OpenLxssMachineKey();
        const auto versionValue = wsl::windows::common::registry::ReadString(key.get(), L"MSI", L"Version");

        VERIFY_ARE_EQUAL(versionValue, WIDEN(WSL_PACKAGE_VERSION));
    }

    TEST_METHOD(MsixInstallerUpgradesWithLogFile)
    {
        // Remove the MSIX package
        UninstallMsix();
        VERIFY_IS_FALSE(IsMsixInstalled());

        // Remove the MSI package
        UninstallMsi();
        VERIFY_IS_FALSE(IsMsiPackageInstalled());
        VERIFY_IS_FALSE(IsMsixInstalled());

        RegistryKeyChange<std::wstring> version(
            HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss\\MSI", L"Version", L"1.0.0");

        auto logFilePath = std::filesystem::current_path() / "msi-install-logs.txt";

        RegistryKeyChange<std::wstring> logFile(
            HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss\\MSI", L"UpgradeLogFile", logFilePath.c_str());

        auto cleanup =
            wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&logFilePath]() { LOG_IF_WIN32_BOOL_FALSE(DeleteFile(logFilePath.c_str())); });

        DeleteProductCode();
        VERIFY_IS_TRUE(GetMsiProductCode().empty());

        // Validate that upgrading the MSIX upgrades the MSI
        InstallMsix();
        WaitForMsiPackageInstall();
        VERIFY_IS_TRUE(IsMsiPackageInstalled());
        VERIFY_IS_TRUE(IsMsixInstalled());
        VERIFY_IS_TRUE(IsMsixInstallerInstalled());

        // Validate that the version got upgraded
        const auto key = wsl::windows::common::registry::OpenLxssMachineKey();
        const auto versionValue = wsl::windows::common::registry::ReadString(key.get(), L"MSI", L"Version");

        VERIFY_ARE_EQUAL(versionValue, WIDEN(WSL_PACKAGE_VERSION));

        // Validate that the log file exists and is not empty
        VERIFY_IS_TRUE(std::filesystem::exists(logFilePath));
        VERIFY_ARE_NOT_EQUAL(std::filesystem::file_size(logFilePath), 0);
    }

    TEST_METHOD(MsixInstallerDoesntDowngrade)
    {
        // Remove the MSIX package
        UninstallMsix();
        VERIFY_IS_FALSE(IsMsixInstalled());

        RegistryKeyChange<std::wstring> change(
            HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss\\MSI", L"Version", L"999.0.0");

        // Validate that upgrading the MSIX doesn't downgrade the MSI
        InstallMsix();
        WaitForInstallerServiceStop();
        VERIFY_IS_TRUE(IsMsiPackageInstalled());
        VERIFY_IS_TRUE(IsMsixInstalled());
        VERIFY_IS_TRUE(IsMsixInstallerInstalled());

        // Validate that the version dit not get upgrade
        const auto key = wsl::windows::common::registry::OpenLxssMachineKey();
        const auto versionValue = wsl::windows::common::registry::ReadString(key.get(), L"MSI", L"Version");

        VERIFY_ARE_EQUAL(versionValue, L"999.0.0");
    }

    TEST_METHOD(MsixUpgradeManual)
    {
        // Registry key to disable auto upgrade on service start
        RegistryKeyChange<DWORD> change(
            HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss\\MSI", L"AutoUpgradeViaMsix", static_cast<DWORD>(0));

        // Remove the MSI package
        UninstallMsi();
        VERIFY_IS_FALSE(IsMsiPackageInstalled());
        VERIFY_IS_FALSE(IsMsixInstalled());

        // Install the installer MSIX
        InstallMsix();

        // Validate that calling wsl.exe triggers the install
        auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(L"echo ok");
        VERIFY_ARE_EQUAL(L"ok\n", output);
        VERIFY_ARE_EQUAL(L"WSL is finishing an upgrade...\r\n", warnings);

        ValidatePackageInstalledProperly();
    }

    TEST_METHOD(MsixUpgradeFails)
    {
        // Remove the MSI package
        UninstallMsi();
        VERIFY_IS_FALSE(IsMsiPackageInstalled());
        VERIFY_IS_FALSE(IsMsixInstalled());

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { InstallMsi(); });

        // Open a handle to wsl.exe in the install directory which will cause the installation to fail.
        //
        // N.B. The file handle will be closed before the cleanup lambda runs.
        std::filesystem::create_directories(m_installedPath);
        wil::unique_hfile fileHandle(::CreateFileW(
            (m_installedPath / WSL_BINARY_NAME).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));

        // Install the installer MSIX
        InstallMsix();

        // Validate that calling wsl.exe triggers the install.
        auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(L"echo ok", -1, nulDevice.get());
        VERIFY_ARE_EQUAL(
            L"\r\nAnother application has exclusive access to the file 'C:\\Program Files\\WSL\\wsl.exe'.  Please shut down all "
            L"other applications, then click Retry.\r\n"
            L"Update failed (exit code: 1603).\r\n"
            L"Error code: Wsl/CallMsi/Install/ERROR_INSTALL_FAILURE\r\n",
            output);
    }

    TEST_METHOD(WslUpdateNoNewVersion)
    {
        constexpr auto endpoint = L"http://127.0.0.1:12345/";
        RegistryKeyChange<std::wstring> change(
            HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", wsl::windows::common::wslutil::c_githubUrlOverrideRegistryValue, endpoint);

        constexpr auto GitHubApiResponse =
            LR"({
                    \"name\": \"1.0.0\",
                    \"created_at\": \"2023-06-14T16:56:30Z\",
                    \"assets\": [
                        {
                            \"url\": \"https://api.github.com/repos/microsoft/WSL/releases/assets/112754644\",
                            \"id\": 112754644,
                            \"name\": \"Microsoft.WSL_1.0.0.0_x64_ARM64.msixbundle\"
                        }
                     ]
                 })";

        UniqueWebServer server(endpoint, GitHubApiResponse);

        auto [out, _] = LxsstuLaunchWslAndCaptureOutput(L"--update");
        VERIFY_ARE_EQUAL(
            out, L"Checking for updates.\r\nThe most recent version of Windows Subsystem for Linux is already installed.\r\n");
    }

    TEST_METHOD(InstallRemovesStaleComRegistration)
    {
        constexpr auto clsid = L"{A9B7A1B9-0671-405C-95F1-E0612CB4CE7E}";

        // Remove the MSI package
        UninstallMsi();
        VERIFY_IS_FALSE(IsMsiPackageInstalled());
        VERIFY_IS_FALSE(IsMsixInstalled());

        // Emulate a leaked packaged COM registration
        auto packagedComClassIndex{wsl::windows::common::registry::OpenKey(
            HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\PackagedCom\\ClassIndex", KEY_CREATE_SUB_KEY | KEY_READ)};

        auto keyExists = [&packagedComClassIndex]() {
            wil::unique_hkey subKey;

            const auto result = RegOpenKeyEx(packagedComClassIndex.get(), clsid, 0, KEY_READ, &subKey);

            return result == NO_ERROR;
        };

        wsl::windows::common::registry::CreateKey(packagedComClassIndex.get(), clsid);

        VERIFY_IS_TRUE(keyExists());

        // Install the package and validate that the registry key was removed.
        InstallMsi();
        VERIFY_IS_TRUE(IsMsiPackageInstalled());
        VERIFY_IS_TRUE(IsMsixInstalled());
        VERIFY_IS_FALSE(IsMsixInstallerInstalled());

        VERIFY_IS_FALSE(keyExists());
        ValidatePackageInstalledProperly();
    }

    TEST_METHOD(InstallremovesStaleServiceRegistration)
    {
        // Remove the MSI package.
        UninstallMsi();
        VERIFY_IS_FALSE(IsMsiPackageInstalled());
        VERIFY_IS_FALSE(IsMsixInstalled());

        // Emulate a leaked packaged service registration.
        const wil::unique_schandle manager{OpenSCManager(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE)};

        wil::unique_schandle service{CreateService(
            manager.get(),
            L"wslservice",
            L"WSL test service",
            GENERIC_ALL,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_DISABLED,
            SERVICE_ERROR_IGNORE,
            L"C:\\windows\\system32\\cmd.exe",
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr)};

        service.reset();

        wil::unique_hkey key = wsl::windows::common::registry::OpenKey(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services", KEY_WRITE);
        THROW_LAST_ERROR_IF(!key);

        wsl::windows::common::registry::WriteString(key.get(), L"wslservice", L"AppUserModelId", L"Dummy");
        key.reset();

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            service.reset(OpenService(manager.get(), L"wslservice", DELETE));
            THROW_LAST_ERROR_IF(!service);

            LOG_IF_WIN32_BOOL_FALSE(DeleteService(service.get()));
        });

        // Install the MSI package. It should delete the wslservice.
        InstallMsi();
        cleanup.release();

        VERIFY_IS_TRUE(IsMsiPackageInstalled());
        VERIFY_IS_TRUE(IsMsixInstalled());

        ValidatePackageInstalledProperly();

        // Validate that the AppUserModelId registry value is gone.
        VERIFY_ARE_EQUAL(wsl::windows::common::registry::ReadString(key.get(), L"wslservice", L"AppUserModelId", L""), L"");
    }

    TEST_METHOD(InstallSetsLspRegistration)
    {
        auto installPath = wsl::windows::common::wslutil::GetMsiPackagePath();
        VERIFY_IS_TRUE(installPath.has_value());

        // Remove the MSI package.
        UninstallMsi();
        VERIFY_IS_FALSE(IsMsiPackageInstalled());
        VERIFY_IS_FALSE(IsMsixInstalled());

        // Validate that LSP flags are not set
        auto getLspFlags = [&installPath](const auto* path) -> std::optional<DWORD> {
            DWORD flags{};
            INT error{};

            if (WSCGetApplicationCategory(path, static_cast<DWORD>(wcslen(path)), nullptr, 0, &flags, &error) == SOCKET_ERROR)
            {
                if (error != WSASERVICE_NOT_FOUND)
                {
                    LogError("WSCGetApplicationCategory failed for: %ls, error: %i", path, error);
                }

                return {};
            }

            return flags;
        };

        const std::vector<LPCWSTR> executables = {L"wsl.exe", L"wslhost.exe", L"wslrelay.exe", L"wslg.exe"};
        for (const auto& e : executables)
        {
            auto fullPath = installPath.value() + e;
            VERIFY_ARE_EQUAL(getLspFlags(fullPath.c_str()).value_or(0), 0);
        }

        // Install the package
        InstallMsi();

        VERIFY_IS_TRUE(IsMsiPackageInstalled());
        VERIFY_IS_TRUE(IsMsixInstalled());
        ValidatePackageInstalledProperly();

        // Validate that the LSP flags were correctly set
        for (const auto& e : executables)
        {
            auto fullPath = installPath.value() + e;
            VERIFY_ARE_EQUAL(getLspFlags(fullPath.c_str()).value_or(0), LSP_SYSTEM);
        }
    }

    TEST_METHOD(InstallClearsExplorerState)
    {
        constexpr auto valueName = L"Attributes";

        // Put the explorer in a state where the WSL shortcut is hidden.

        const auto key = wsl::windows::common::registry::CreateKey(
            HKEY_CURRENT_USER,
            LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\CLSID\{B2B4A4D1-2754-4140-A2EB-9A76D9D7CDC6}\ShellFolder)",
            KEY_READ | KEY_WRITE);

        wsl::windows::common::registry::WriteDword(key.get(), nullptr, valueName, SFGAO_NONENUMERATED);

        // Install the package
        InstallMsi();

        VERIFY_IS_TRUE(IsMsiPackageInstalled());
        VERIFY_IS_TRUE(IsMsixInstalled());
        ValidatePackageInstalledProperly();

        // Validate that the installer removed the problematic flag

        VERIFY_ARE_EQUAL(wsl::windows::common::registry::ReadDword(key.get(), nullptr, valueName, 0), 0);
    }

    TEST_METHOD(InstallUnprotectsKeys)
    {
        const auto installPath = wsl::windows::common::wslutil::GetMsiPackagePath();
        VERIFY_IS_TRUE(installPath.has_value());

        constexpr auto keyPath = LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\IdListAliasTranslations\WSL)";

        // Create a protected key that the installer will write to
        {
            auto [localAdministratorsSid, adminSidBuffer] =
                wsl::windows::common::security::CreateSid(SECURITY_NT_AUTHORITY, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);

            EXPLICIT_ACCESS aces[2];
            aces[0].grfAccessMode = SET_ACCESS;
            aces[0].grfAccessPermissions = KEY_READ;
            aces[0].grfInheritance = NO_INHERITANCE;
            BuildTrusteeWithSid(&aces[0].Trustee, localAdministratorsSid);

            aces[1].grfAccessMode = GRANT_ACCESS;
            aces[1].grfAccessPermissions = KEY_ALL_ACCESS;
            aces[1].grfInheritance = NO_INHERITANCE;
            std::wstring trustedInstaller = L"NT Service\\TrustedInstaller";
            BuildTrusteeWithName(&aces[1].Trustee, trustedInstaller.data());

            wsl::windows::common::security::unique_acl newAcl{};
            THROW_IF_WIN32_ERROR(SetEntriesInAcl(2, aces, nullptr, &newAcl));

            SECURITY_DESCRIPTOR newDescriptor{};
            THROW_IF_WIN32_BOOL_FALSE(InitializeSecurityDescriptor(&newDescriptor, SECURITY_DESCRIPTOR_REVISION));
            THROW_IF_WIN32_BOOL_FALSE(SetSecurityDescriptorDacl(&newDescriptor, true, newAcl.get(), false));

            auto restore = wsl::windows::common::security::AcquirePrivileges({SE_BACKUP_NAME, SE_RESTORE_NAME});

            const auto key =
                wsl::windows::common::registry::CreateKey(HKEY_LOCAL_MACHINE, keyPath, KEY_ALL_ACCESS, nullptr, REG_OPTION_BACKUP_RESTORE);
            THROW_IF_WIN32_ERROR(RegSetKeySecurity(key.get(), DACL_SECURITY_INFORMATION, &newDescriptor));
        }

        VERIFY_IS_TRUE(SfcIsKeyProtected(HKEY_LOCAL_MACHINE, keyPath, KEY_WOW64_64KEY));

        // Install the package
        InstallMsi();

        VERIFY_IS_TRUE(IsMsiPackageInstalled());
        VERIFY_IS_TRUE(IsMsixInstalled());
        ValidatePackageInstalledProperly();

        // Verify that key was unprotected.
        VERIFY_IS_FALSE(SfcIsKeyProtected(HKEY_LOCAL_MACHINE, keyPath, KEY_WOW64_64KEY));
    }

    void CallWslUpdateViaMsi()
    {

#ifdef WSL_DEV_THIN_MSI_PACKAGE

        LogSkipped("This test case cannot run with a thin MSI package");
        return;

#endif

        constexpr auto endpoint = L"http://127.0.0.1:12345/";
        RegistryKeyChange<std::wstring> change(
            HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", wsl::windows::common::wslutil::c_githubUrlOverrideRegistryValue, endpoint);

        constexpr auto GitHubApiResponse =
            LR"({
                    \"name\": \"999.0.0\",
                    \"created_at\": \"2023-06-14T16:56:30Z\",
                    \"assets\": [
                        {
                            \"url\": \"http://127.0.0.1:12346/wsl.testpackage.x64.msi\",
                            \"id\": 0,
                            \"name\": \"wsl.testpackage.x64.msi\"
                        }
                     ]
                 })";

        UniqueWebServer apiServer(endpoint, GitHubApiResponse);
        UniqueWebServer fileServer(L"http://127.0.0.1:12346/", std::filesystem::path(m_msiPath));

        // DeleteProductCode();
        // TODO: It would be good to remove something to validate that the MSI actually gets installed,
        // but this doesn't work during the tests because the ProductCode is the same so the components
        // don't actually get reinstalled and we can't use REINSTALLMODE because this would skip component removal
        // during upgrade.

        // The MSI upgrade can send a ctrl-c to wsl.exe, so create a new console so the test process doesn't receive the ctrl-c.
        const auto commandLine = LxssGenerateWslCommandLine(L"--update");
        wsl::windows::common::SubProcess process(nullptr, commandLine.data(), CREATE_NEW_CONSOLE);
        process.SetShowWindow(SW_HIDE);
        LogInfo("wsl --update exited with: %lu", process.Run());

        // wsl --update isn't synchronous since wsl.exe will be killed during the installation.
        WaitForMsiPackageInstall();

        ValidatePackageInstalledProperly();
        VERIFY_IS_TRUE(IsMsixInstalled());
        VERIFY_IS_TRUE(!GetMsiProductCode().empty());
    }

    TEST_METHOD(WslUpdateViaMsi)
    {
        CallWslUpdateViaMsi();
    }

    TEST_METHOD(WslUpdateViaMsix)
    {
        constexpr auto endpoint = L"http://127.0.0.1:12345/";
        RegistryKeyChange<std::wstring> change(
            HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", wsl::windows::common::wslutil::c_githubUrlOverrideRegistryValue, endpoint);

        constexpr auto GitHubApiResponse =
            LR"({
                    \"name\": \"999.0.0\",
                    \"created_at\": \"2023-06-14T16:56:30Z\",
                    \"assets\": [
                        {
                            \"url\": \"http://127.0.0.1:12346/wsl.testpackage.msixbundle\",
                            \"id\": 0,
                            \"name\": \"wsl.testpackage.x64.msixbundle\"
                        }
                     ]
                 })";

        UniqueWebServer apiServer(endpoint, GitHubApiResponse);
        UniqueWebServer fileServer(L"http://127.0.0.1:12346/", std::filesystem::path(m_msixPackagePath));

        UninstallMsix();
        VERIFY_IS_FALSE(IsMsixInstalled());

        const auto installLocation = wsl::windows::common::wslutil::GetMsiPackagePath();
        VERIFY_IS_TRUE(installLocation.has_value());
        auto cmd = installLocation.value() + L"\\wsl.exe --update";
        LxsstuRunCommand(cmd.data()); // Ignore the error code since wsl.exe will be killed by msiexec

        VERIFY_IS_TRUE(IsMsiPackageInstalled());
        VERIFY_IS_TRUE(IsMsixInstalled());
        VERIFY_IS_TRUE(IsMsixInstallerInstalled());
        ValidatePackageInstalledProperly();
    }

    static bool WslSettingsProtocolAssociationExists()
    {
        wil::com_ptr<IEnumAssocHandlers> enumAssocHandlers;
        if (FAILED(SHAssocEnumHandlersForProtocolByApplication(L"wsl-settings", IID_PPV_ARGS(&enumAssocHandlers))))
        {
            return false;
        }

        ULONG elementsReturned;
        wil::com_ptr<IAssocHandler> currentAssoc;
        while (SUCCEEDED(enumAssocHandlers->Next(1, &currentAssoc, &elementsReturned)))
        {
            wil::unique_cotaskmem_string name;
            if (FAILED(currentAssoc->GetName(&name)))
            {
                LogError("Failed to get association name, continuing...");
                continue;
            }

            if (::_wcsicmp(name.get(), L"WSL Settings") != 0)
            {
                return true;
            }
        }

        return false;
    }

    void VerifyWslSettingsProtocolAssociationExistsWithRetry()
    {
        VERIFY_NO_THROW(wsl::shared::retry::RetryWithTimeout<void>(
            [&]() { THROW_HR_IF(E_UNEXPECTED, !WslSettingsProtocolAssociationExists()); }, std::chrono::seconds(1), std::chrono::minutes(2)));
    }

    TEST_METHOD(WslValidateWslSettingsProtocol)
    {
        WSL_SETTINGS_TEST();

        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        VerifyWslSettingsProtocolAssociationExistsWithRetry();

        UninstallMsi();
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

        wil::com_ptr<IEnumAssocHandlers> enumAssocHandlers;
        const HRESULT hr = SHAssocEnumHandlersForProtocolByApplication(L"wsl-settings", IID_PPV_ARGS(&enumAssocHandlers));
        if (FAILED(hr))
        {
            VERIFY_ARE_EQUAL(hr, HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION));
        }
        else
        {
            VERIFY_IS_FALSE(WslSettingsProtocolAssociationExists());
        }

        InstallMsi();
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        VerifyWslSettingsProtocolAssociationExistsWithRetry();
    }
};