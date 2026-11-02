// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "Common.h"
#include "WSLCLocalhost.h"
#include "../../../src/windows/wslcsession/WSLCStartup.h"

namespace WSLCLocalhostTests {

using namespace wsl::shared::wslc;
using wsl::windows::service::wslc::CheckGuestMountResult;
using wsl::windows::service::wslc::WaitForDockerStartup;

class WSLCLocalhostUnitTests
{
    WSLC_TEST_CLASS(WSLCLocalhostUnitTests)

    static auto CreateTestDirectory()
    {
        GUID id{};
        VERIFY_SUCCEEDED(CoCreateGuid(&id));
        auto path = std::filesystem::temp_directory_path() /
                    std::format(L"wslc-localhost-{}", wsl::shared::string::GuidToString<wchar_t>(id));
        VERIFY_IS_TRUE(std::filesystem::create_directory(path));
        auto cleanup = wil::scope_exit([path]() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
            if (error)
            {
                LogError("Failed to remove localhost test directory: %hs", error.message().c_str());
            }
        });
        return std::pair{std::move(path), std::move(cleanup)};
    }

    static void WriteFile(const std::filesystem::path& Path, std::string_view Contents)
    {
        std::ofstream output;
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output.open(Path, std::ios::binary);
        output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
        output.close();
    }

    TEST_METHOD(Hosts_ReplacesNonLoopbackLocalhostAndPreservesOtherEntries)
    {
        const auto result = ConfigureLocalhostHosts(
            "# guest entries\n192.0.2.1 LOCALHOST custom # keep\n::1 localhost ip6-localhost ip6-loopback extra\n"
            "203.0.113.2 service");
        VERIFY_ARE_EQUAL(
            "127.0.0.1\tlocalhost\n::1\tlocalhost ip6-localhost ip6-loopback\n"
            "# guest entries\n192.0.2.1\tcustom\t# keep\n::1\textra\n203.0.113.2 service\n",
            result);
        VERIFY_ARE_EQUAL(result, ConfigureLocalhostHosts(result));
    }

    TEST_METHOD(NameService_UsesFilesBeforeDnsAndRemovesDuplicateHostsRules)
    {
        const auto result =
            ConfigureLocalhostNameService("passwd: files\n hosts : dns [!UNAVAIL=return] files # keep\nhosts: dns # duplicate\n");
        VERIFY_ARE_EQUAL("passwd: files\nhosts: files dns # keep\n# duplicate\n", result);
        VERIFY_ARE_EQUAL(result, ConfigureLocalhostNameService(result));
    }

    TEST_METHOD(Configuration_CreatesAndPopulatesFiles)
    {
        auto [directory, cleanup] = CreateTestDirectory();
        const auto hosts = directory / "hosts";
        const auto nsswitch = directory / "nsswitch.conf";
        WriteFile(nsswitch, {});
        ConfigureLocalhostFile(hosts, ConfigureLocalhostHosts);
        ConfigureLocalhostFile(nsswitch, ConfigureLocalhostNameService);
        VERIFY_ARE_EQUAL(
            "127.0.0.1\tlocalhost\n::1\tlocalhost ip6-localhost ip6-loopback\n", wsl::shared::string::ReadFile<char>(hosts.c_str()));
        VERIFY_ARE_EQUAL("hosts: files dns\n", wsl::shared::string::ReadFile<char>(nsswitch.c_str()));
    }

    TEST_METHOD(Configuration_FailedReadDoesNotOverwriteExistingFile)
    {
        auto [directory, cleanup] = CreateTestDirectory();
        const auto hosts = directory / "hosts";
        constexpr auto c_original = "192.0.2.1 custom\n";
        WriteFile(hosts, c_original);

        wil::unique_hfile locked{CreateFileW(hosts.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        VERIFY_IS_TRUE(static_cast<bool>(locked));
        VERIFY_THROWS_SPECIFIC(
            ConfigureLocalhostFile(hosts, ConfigureLocalhostHosts), std::ios_base::failure, [](const auto&) { return true; });
        locked.reset();
        VERIFY_ARE_EQUAL(c_original, wsl::shared::string::ReadFile<char>(hosts.c_str()));
    }

    TEST_METHOD(Configuration_FailedWriteIsReported)
    {
        auto [directory, cleanup] = CreateTestDirectory();
        const auto hosts = directory / "hosts";
        constexpr auto c_original = "192.0.2.1 custom\n";
        WriteFile(hosts, c_original);
        VERIFY_WIN32_BOOL_SUCCEEDED(SetFileAttributesW(hosts.c_str(), FILE_ATTRIBUTE_READONLY));
        auto resetAttributes =
            wil::scope_exit([&]() { LOG_LAST_ERROR_IF(!SetFileAttributesW(hosts.c_str(), FILE_ATTRIBUTE_NORMAL)); });

        VERIFY_THROWS_SPECIFIC(
            ConfigureLocalhostFile(hosts, ConfigureLocalhostHosts), std::ios_base::failure, [](const auto&) { return true; });
        VERIFY_ARE_EQUAL(c_original, wsl::shared::string::ReadFile<char>(hosts.c_str()));
    }

    TEST_METHOD(Configuration_AlreadyConfiguredFileDoesNotRequireWriteAccess)
    {
        auto [directory, cleanup] = CreateTestDirectory();
        const auto hosts = directory / "hosts";
        const auto configured = ConfigureLocalhostHosts({});
        WriteFile(hosts, configured);
        VERIFY_WIN32_BOOL_SUCCEEDED(SetFileAttributesW(hosts.c_str(), FILE_ATTRIBUTE_READONLY));
        auto resetAttributes =
            wil::scope_exit([&]() { LOG_LAST_ERROR_IF(!SetFileAttributesW(hosts.c_str(), FILE_ATTRIBUTE_NORMAL)); });

        ConfigureLocalhostFile(hosts, ConfigureLocalhostHosts);
        VERIFY_ARE_EQUAL(configured, wsl::shared::string::ReadFile<char>(hosts.c_str()));
    }

    TEST_METHOD(Initialization_FailedResolverFilesReportTheAffectedPath)
    {
        for (const auto& [failure, path] :
             {std::pair{WSLC_MOUNT_RESULT::LocalhostConfigurationFailure::Hosts, "/etc/hosts"},
              std::pair{WSLC_MOUNT_RESULT::LocalhostConfigurationFailure::NameServiceSwitch, "/etc/nsswitch.conf"}})
        {
            WSLC_MOUNT_RESULT response{};
            response.Result = EACCES;
            response.LocalhostFailure = failure;
            HRESULT result{};
            {
                wsl::windows::common::COMServiceExecutionContext context;
                result = wil::ResultFromException([&]() { CheckGuestMountResult(response); });
            }
            VERIFY_ARE_EQUAL(E_FAIL, result);
            ValidateCOMErrorMessage(wsl::shared::Localization::MessageWslcLocalhostInitFailed(path, EACCES));
        }
    }

    TEST_METHOD(Startup_ReadySucceedsWithoutCheckingExitCode)
    {
        wil::unique_event ready{wil::EventOptions::ManualReset};
        wil::unique_event exited{wil::EventOptions::ManualReset};
        wil::unique_event terminating{wil::EventOptions::ManualReset};
        ready.SetEvent();
        unsigned exitCodeReads = 0;
        WaitForDockerStartup(ready.get(), exited.get(), terminating.get(), 0, [&]() {
            ++exitCodeReads;
            return 42;
        });
        VERIFY_ARE_EQUAL(0u, exitCodeReads);
    }

    TEST_METHOD(Startup_TimeoutReportsReadinessFailureWithoutDnsAttribution)
    {
        wil::unique_event ready{wil::EventOptions::ManualReset};
        wil::unique_event exited{wil::EventOptions::ManualReset};
        wil::unique_event terminating{wil::EventOptions::ManualReset};
        HRESULT result{};
        {
            wsl::windows::common::COMServiceExecutionContext context;
            result = wil::ResultFromException(
                [&]() { WaitForDockerStartup(ready.get(), exited.get(), terminating.get(), 0, []() { return 42; }); });
        }
        VERIFY_ARE_EQUAL(HRESULT_FROM_WIN32(ERROR_TIMEOUT), result);
        ValidateCOMErrorMessage(wsl::shared::Localization::MessageWslcDockerStartupTimedOut(0));
    }

    TEST_METHOD(Startup_ProcessExitTakesPrecedenceOverReadiness)
    {
        wil::unique_event ready{wil::EventOptions::ManualReset};
        wil::unique_event exited{wil::EventOptions::ManualReset};
        wil::unique_event terminating{wil::EventOptions::ManualReset};
        ready.SetEvent();
        exited.SetEvent();
        HRESULT result{};
        {
            wsl::windows::common::COMServiceExecutionContext context;
            result = wil::ResultFromException(
                [&]() { WaitForDockerStartup(ready.get(), exited.get(), terminating.get(), 0, []() { return 42; }); });
        }
        VERIFY_ARE_EQUAL(E_FAIL, result);
        ValidateCOMErrorMessage(wsl::shared::Localization::MessageWslcDockerExitedBeforeReady(42));
    }

    TEST_METHOD(Startup_CancellationTakesPrecedenceOverReadiness)
    {
        wil::unique_event ready{wil::EventOptions::ManualReset};
        wil::unique_event exited{wil::EventOptions::ManualReset};
        wil::unique_event terminating{wil::EventOptions::ManualReset};
        ready.SetEvent();
        terminating.SetEvent();
        VERIFY_THROWS_SPECIFIC(
            WaitForDockerStartup(ready.get(), exited.get(), terminating.get(), 0, []() { return 42; }),
            wil::ResultException,
            [](const wil::ResultException& error) { return error.GetErrorCode() == E_ABORT; });
    }
};

} // namespace WSLCLocalhostTests
