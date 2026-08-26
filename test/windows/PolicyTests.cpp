/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PolicyTests.cpp

Abstract:

    This file contains test cases for WSL policies.

--*/

#include "precomp.h"
#include <fstream>
#include "Common.h"
#include "registry.hpp"
#include "wslpolicies.h"

using namespace wsl::windows::policies;
using namespace wsl::windows::common::registry;

class PolicyTest
{
    WSL_TEST_CLASS(PolicyTest)

    bool m_initialized = false;

    TEST_CLASS_SETUP(TestClassSetup)
    {
        const auto policies = OpenKey(HKEY_LOCAL_MACHINE, ROOT_POLICIES_KEY, KEY_CREATE_SUB_KEY, 0);
        VERIFY_IS_TRUE(!!policies);

        const auto wslPolicies = CreateKey(policies.get(), L"WSL");
        VERIFY_IS_TRUE(!!wslPolicies);

        VERIFY_ARE_EQUAL(LxsstuInitialize(FALSE), TRUE);
        m_initialized = true;
        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        if (m_initialized)
        {
            LxsstuUninitialize(FALSE);
        }

        return true;
    }

    static auto SetPolicy(LPCWSTR Name, DWORD Value)
    {
        return RegistryKeyChange(HKEY_LOCAL_MACHINE, c_registryKey, Name, Value);
    }

    // Writes the supplied entries under the WSLContainerRegistryAllowlist sub-key as REG_SZ
    // values named "AllowedRegistry1", "AllowedRegistry2", ... (matching what the GP editor
    // writes for the ADMX `<list valuePrefix="AllowedRegistry"/>` policy) and deletes the
    // sub-key when the returned scope exits.
    static auto SetRegistryAllowlist(std::initializer_list<std::wstring_view> entries)
    {
        const auto policies = OpenKey(HKEY_LOCAL_MACHINE, c_registryKey, KEY_ALL_ACCESS);

        // Drop any pre-existing sub-key so stale `AllowedRegistryN` values from a previous
        // (possibly interrupted) test run can't leak into this one.
        DeleteKey(policies.get(), c_wslContainerRegistryAllowlist);

        const auto subKey = CreateKey(policies.get(), c_wslContainerRegistryAllowlist);
        DWORD index = 1;
        for (const auto& entry : entries)
        {
            const auto name = std::format(L"AllowedRegistry{}", index++);
            const std::wstring data{entry};
            WriteString(subKey.get(), nullptr, name.c_str(), data.c_str());
        }
        return wil::scope_exit([] {
            try
            {
                const auto policies = OpenKey(HKEY_LOCAL_MACHINE, c_registryKey, KEY_ALL_ACCESS);
                DeleteKey(policies.get(), c_wslContainerRegistryAllowlist);
            }
            CATCH_LOG()
        });
    }

    static void ValidateWarnings(const std::wstring& expectedWarnings, bool pattern = false)
    {
        auto [output, warnings] = LxsstuLaunchWslAndCaptureOutput(L"echo ok");
        VERIFY_ARE_EQUAL(L"ok\n", output);

        if (pattern)
        {
            if (!PathMatchSpec(warnings.c_str(), expectedWarnings.c_str()))
            {
                LogError("Warning '%ls' didn't match pattern '%ls'", warnings.c_str(), expectedWarnings.c_str());
                VERIFY_FAIL();
            }
        }
        else
        {
            VERIFY_ARE_EQUAL(expectedWarnings, warnings);
        }
    };

    WSL2_TEST_METHOD(MountPolicyAllowed)
    {
        SKIP_TEST_ARM64();
        auto revert = SetPolicy(c_allowDiskMount, 1);
        ValidateOutput(
            L"--mount DoesNotExist",
            FormatErrorMessage(
                L"Failed to attach disk 'DoesNotExist' to WSL2: The system cannot find the file specified. ",
                L"Wsl/Service/AttachDisk/MountDisk/HCS/ERROR_FILE_NOT_FOUND"));
    }

    WSL2_TEST_METHOD(MountPolicyDisabled)
    {
        SKIP_TEST_ARM64();
        auto revert = SetPolicy(c_allowDiskMount, 0);
        ValidateOutput(
            L"--mount DoesNotExist",
            FormatErrorMessage(L"wsl.exe --mount is disabled by the computer policy.", L"Wsl/Service/WSL_E_DISK_MOUNT_DISABLED"));
    }

    void ValidatePolicy(LPCWSTR Name, LPCWSTR Config, LPCWSTR ExpectedWarnings, const std::function<void(DWORD)>& Validate = [](auto) {})
    {
        WslConfigChange config(LxssGenerateTestConfig() + Config);

        // Validate behavior with policy allowed
        {
            auto revert = SetPolicy(Name, 1);
            WslShutdown();

            ValidateWarnings(L""); // Expect no warnings
            Validate(1);
        }

        // Validate behavior with policy disabled
        {
            auto revert = SetPolicy(Name, 0);
            WslShutdown();

            ValidateWarnings(ExpectedWarnings);
            Validate(0);
        }

        // Validate behavior with an invalid policy value
        {
            auto revert = SetPolicy(Name, 12);
            WslShutdown();

            ValidateWarnings(L"");
            Validate(12);
        }
    }

    WSL2_TEST_METHOD(KernelCommandLine)
    {
        auto validate = [](DWORD policyValue) {
            auto [commandLine, _] = LxsstuLaunchWslAndCaptureOutput(L"cat /proc/cmdline");

            if (policyValue == 0)
            {
                VERIFY_IS_FALSE(commandLine.find(L"dummy-cmd-arg") != std::wstring::npos);
            }
            else
            {
                VERIFY_IS_TRUE(commandLine.find(L"dummy-cmd-arg") != std::wstring::npos);
            }
        };

        ValidatePolicy(
            c_allowCustomKernelCommandLineUserSetting,
            L"kernelCommandLine=dummy-cmd-arg",
            L"wsl: The .wslconfig setting 'wsl2.kernelCommandLine' is disabled by the computer policy.\r\n",
            validate);
    }

    WSL2_TEST_METHOD(NestedVirtualization)
    {
        SKIP_TEST_ARM64();
        WINDOWS_11_TEST_ONLY();

        ValidatePolicy(
            c_allowNestedVirtualizationUserSetting,
            L"nestedVirtualization=true",
            L"wsl: The .wslconfig setting 'wsl2.nestedVirtualization' is disabled by the computer policy.\r\n");
    }

    WSL2_TEST_METHOD(KernelDebugging)
    {
        WINDOWS_11_TEST_ONLY();

        ValidatePolicy(
            c_allowKernelDebuggingUserSetting,
            L"kernelDebugPort=1234",
            L"wsl: The .wslconfig setting 'wsl2.kernelDebugPort' is disabled by the computer policy.\r\n");
    }

    WSL2_TEST_METHOD(CustomKernel)
    {
        const std::wstring wslConfigPath = wsl::windows::common::helpers::GetWslConfigPath();
        const std::wstring nonExistentFile = L"DoesNotExist";
        WslConfigChange config(LxssGenerateTestConfig({.kernel = nonExistentFile.c_str(), .kernelModules = nonExistentFile.c_str()}));

        {
            auto revert = SetPolicy(c_allowCustomKernelUserSetting, 1);
            WslShutdown();

            ValidateOutput(
                L"echo ok",
                FormatErrorMessage(
                    wsl::shared::Localization::MessageCustomKernelNotFound(wslConfigPath, nonExistentFile),
                    L"Wsl/Service/CreateInstance/CreateVm/WSL_E_CUSTOM_KERNEL_NOT_FOUND"));
        }

        // Disable the custom kernel policy and validate that the expected warnings are shown.
        {
            auto revert = SetPolicy(c_allowCustomKernelUserSetting, 0);
            WslShutdown();

            const auto kernelWarning =
                std::format(L"wsl: {}\r\n", wsl::shared::Localization::MessageSettingOverriddenByPolicy(L"wsl2.kernel"));
            const auto modulesWarning =
                std::format(L"wsl: {}\r\n", wsl::shared::Localization::MessageSettingOverriddenByPolicy(L"wsl2.kernelModules"));

            ValidateWarnings(std::format(L"{}{}", kernelWarning, modulesWarning));

            config.Update(LxssGenerateTestConfig({.kernel = nonExistentFile.c_str()}));
            ValidateWarnings(kernelWarning);

            config.Update(LxssGenerateTestConfig({.kernelModules = nonExistentFile.c_str()}));
            ValidateWarnings(modulesWarning);
        }
    }

    WSL2_TEST_METHOD(CustomSystemDistro)
    {
        WslConfigChange config(LxssGenerateTestConfig() + L"systemDistro=DoesNotExist");
        const std::wstring wslConfigPath = wsl::windows::common::helpers::GetWslConfigPath();

        {
            auto revert = SetPolicy(c_allowCustomSystemDistroUserSetting, 1);
            WslShutdown();

            ValidateOutput(
                L"echo ok",
                FormatErrorMessage(
                    L"The custom system distribution specified in " + wslConfigPath + L" was not found or is not the correct format.",
                    L"Wsl/Service/CreateInstance/CreateVm/WSL_E_CUSTOM_SYSTEM_DISTRO_ERROR"));
        }

        {
            auto revert = SetPolicy(c_allowCustomSystemDistroUserSetting, 0);
            WslShutdown();

            ValidateWarnings(L"wsl: The .wslconfig setting 'wsl2.systemDistro' is disabled by the computer policy.\r\n");
        }
    }

    WSL2_TEST_METHOD(CustomNetworkingMode)
    {
        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Consomme}));

        {
            auto revert = SetPolicy(c_allowCustomNetworkingModeUserSetting, 1);
            WslShutdown();

            ValidateWarnings(L"");
        }

        {
            auto revertCustomMode = SetPolicy(c_allowCustomNetworkingModeUserSetting, 0);
            WslShutdown();

            ValidateWarnings(L"wsl: The .wslconfig setting 'wsl2.networkingMode' is disabled by the computer policy.\r\n");

            // Validate that no warnings are shown for NAT or None
            config.Update(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Nat}));
            ValidateWarnings(L"");

            config.Update(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::None}));
            ValidateWarnings(L"");

            // Validate that no warnings are shown if the default networking mode is set to the same value as .wslconfig.
            auto revertDefault = SetPolicy(c_defaultNetworkingMode, static_cast<DWORD>(wsl::core::NetworkingMode::Consomme));
            config.Update(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::Consomme}));
            ValidateWarnings(L"");
        }
    }

    WSL2_TEST_METHOD(DebugShell)
    {
        auto revert = SetPolicy(c_allowDebugShellUserSetting, 0);
        WslShutdown();

        // Only testing the negative case since the debug shell is difficult to programmatically exit.

        WslKeepAlive keepAlive;
        ValidateOutput(L"--debug-shell", L"The debug shell is disabled by the computer policy.\r\n", L"", 1);
    }

    TEST_METHOD(WSL1)
    {
        // Test policy registry key with allow key explicitly set.
        {
            auto revert = SetPolicy(c_allowWSL1, 1);
            WslShutdown();

            ValidateWarnings(L"");
        }

        // Disable WSL1.
        {
            auto revert = SetPolicy(c_allowWSL1, 0);
            WslShutdown();

            // If running as WSL2, attempt to convert the distro to WSL1. If running as WSL1, attempt to run a command.
            if (LxsstuVmMode())
            {
                ValidateOutput(
                    L"--set-version " LXSS_DISTRO_NAME_TEST_L L" 1",
                    FormatErrorMessage(L"WSL1 is disabled by the computer policy.", L"Wsl/Service/WSL_E_WSL1_DISABLED"));
            }
            else
            {
                ValidateOutput(
                    L"echo ok",
                    FormatErrorMessage(
                        L"WSL1 is disabled by the computer policy.\r\nPlease run 'wsl.exe "
                        L"--set-version " LXSS_DISTRO_NAME_TEST_L L" 2' to upgrade to WSL2.",
                        L"Wsl/Service/CreateInstance/WSL_E_WSL1_DISABLED"));
            }
        }
    }

    TEST_METHOD(DisableWsl)
    {
        // N.B. Modifying one of the policy registry keys triggers a registry watcher in the service.
        //      Retry for up to 30 seconds to ensure the registry watcher has time to take effect.
        auto createInstance = [&](HRESULT expectedResult) {
            HRESULT result;
            const auto stop = std::chrono::steady_clock::now() + std::chrono::seconds{30};
            for (;;)
            {
                wil::com_ptr<ILxssUserSession> session;
                result = CoCreateInstance(CLSID_LxssUserSession, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&session));
                if (result == expectedResult || std::chrono::steady_clock::now() > stop)
                {
                    break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds{250});
            }

            VERIFY_ARE_EQUAL(expectedResult, result);
            if (SUCCEEDED(result))
            {
                VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"/bin/true"), 0u);
            }
            else
            {
                auto [output, _] = LxsstuLaunchWslAndCaptureOutput(L"/bin/true", -1);
                VERIFY_ARE_EQUAL(
                    output,
                    FormatErrorMessage(
                        L"This program is blocked by group policy. For more information, contact your system administrator. ",
                        L"Wsl/ERROR_ACCESS_DISABLED_BY_POLICY"));
            }
        };

        // Set the policy registry key and validate that user session creation returns the expected result,
        // then delete the key and ensure user session can be created.
        auto testPolicy = [&](LPCWSTR policy, HRESULT expectedResult, bool restartService) {
            {
                auto revert = SetPolicy(policy, 0);
                if (restartService)
                {
                    RestartWslService();
                }

                createInstance(expectedResult);
            }

            if (restartService)
            {
                RestartWslService();
            }
            createInstance(S_OK);
        };

        for (const auto restartService : {false, true})
        {
            // Ensure the top-level disable WSL policy works.
            testPolicy(wsl::windows::policies::c_allowWSL, HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY), restartService);

            // Verify the disable inbox WSL policy does not block lifted.
            testPolicy(wsl::windows::policies::c_allowInboxWSL, S_OK, restartService);
        }

        // Delete and recreate the key without restarting the service to ensure the registry watcher continues to work.
        wsl::windows::common::registry::DeleteKey(HKEY_LOCAL_MACHINE, wsl::windows::policies::c_registryKey);
        auto key = wsl::windows::common::registry::CreateKey(HKEY_LOCAL_MACHINE, wsl::windows::policies::c_registryKey);
        testPolicy(wsl::windows::policies::c_allowWSL, HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY), false);
    }

    WSL2_TEST_METHOD(DefaultNetworkingMode)
    {
        WslConfigChange config(LxssGenerateTestConfig());

        {
            auto revert = SetPolicy(c_defaultNetworkingMode, static_cast<DWORD>(wsl::core::NetworkingMode::None));
            WslShutdown();

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"wslinfo --networking-mode | grep -iF 'none'"), 0u);
        }

        {
            auto revert = SetPolicy(c_defaultNetworkingMode, static_cast<DWORD>(wsl::core::NetworkingMode::Consomme));
            WslShutdown();

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"wslinfo --networking-mode | grep -iF 'consomme'"), 0u);
        }
    }

    // Build the absolute path to the installed wslc.exe.
    static std::wstring GetWslcExePath()
    {
        auto msiPath = wsl::windows::common::wslutil::GetMsiPackagePath();
        THROW_HR_IF_MSG(E_UNEXPECTED, !msiPath.has_value(), "MSI install location not found in registry; is WSL installed?");
        return (std::filesystem::path(*msiPath) / L"wslc.exe").wstring();
    }

    // Verifies AllowWSLContainer=0 gates the WSLCSessionManager COM factory itself, so that
    // every method (including GetVersion) is unreachable when the policy disables containers.
    WSLC_TEST_METHOD(WSLContainerDisabled)
    {
        auto revert = SetPolicy(c_allowWSLContainer, 0);

        wil::com_ptr<IWSLCSessionManager> sessionManager;
        HRESULT hr = CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager));
        VERIFY_ARE_EQUAL(WSLC_E_CONTAINER_DISABLED, hr);
        VERIFY_IS_NULL(sessionManager.get());
    }

    // Verifies AllowWSLContainer=0 gates wslc.exe at startup with a friendly message that is
    // surfaced on stderr (and not stdout). Locks down both the exact rendered text and the
    // handle the message is written to so future regressions show up here.
    WSLC_TEST_METHOD(WSLContainerDisabledCli)
    {
        auto revert = SetPolicy(c_allowWSLContainer, 0);

        std::wstring cmd = L"\"" + GetWslcExePath() + L"\" container ls";
        auto [stdoutText, stderrText, exitCode] = LxsstuLaunchCommandAndCaptureOutputWithResult(cmd.data(), nullptr, nullptr);

        VERIFY_ARE_EQUAL(1, exitCode);

        // The disabled message must go to stderr only -- never to stdout.
        VERIFY_ARE_EQUAL(L"", stdoutText);

        // The wslc CLI renders failures via MessageErrorCode and
        // PrintMessage adds a trailing newline; line endings are \r\n through console pipes.
        const auto expected =
            FormatErrorMessage(wsl::shared::Localization::MessageWSLContainerDisabled(), L"WSLC_E_CONTAINER_DISABLED");
        VERIFY_ARE_EQUAL(expected, stderrText);
    }

    // Verifies the WSLContainerRegistryAllowlist denies image pulls from registries not in the
    // allowlist.
    WSLC_TEST_METHOD(RegistryAllowlistDenies)
    {
        // Allowlist contains ONLY mcr.microsoft.com -- pulling docker.io must be denied.
        auto revert = SetRegistryAllowlist({L"mcr.microsoft.com"});

        std::wstring cmd = L"\"" + GetWslcExePath() + L"\" image pull alpine:latest";
        auto [stdoutText, stderrText, exitCode] = LxsstuLaunchCommandAndCaptureOutputWithResult(cmd.data(), nullptr, nullptr);

        VERIFY_ARE_NOT_EQUAL(0, exitCode);
        VERIFY_ARE_EQUAL(L"", stdoutText);

        const auto expected = FormatErrorMessage(
            wsl::shared::Localization::MessageRegistryBlockedByPolicy(L"docker.io"), L"WSLC_E_REGISTRY_BLOCKED_BY_POLICY");
        VERIFY_ARE_EQUAL(expected, stderrText);
    }

    // Two variants: BuildKit echoes the caller's Dockerfile spelling in the "failed to solve" prefix.
    static constexpr auto c_denialPatternExplicitAlpine =
        "*failed to solve: docker.io/library/alpine:latest: could not resolve image due to policy: "
        "source \"docker-image://docker.io/library/alpine:latest\" denied by policy: source denied by policy*";
    static constexpr auto c_denialPatternImplicitAlpine =
        "*failed to solve: alpine:latest: could not resolve image due to policy: "
        "source \"docker-image://docker.io/library/alpine:latest\" denied by policy: source denied by policy*";

    // Verifies WSLContainerRegistryAllowlist blocks `wslc image build` when the FROM base image
    // isn't in the allowlist. Matches the `RegistryAllowlistDenies` pull test.
    WSLC_TEST_METHOD(RegistryAllowlistBlocksImageBuild)
    {
        auto revert = SetRegistryAllowlist({L"mcr.microsoft.com"});

        auto [exitCode, output] = RunImageBuild(L"FROM docker.io/library/alpine:latest\n", L"wsl-policy-build-blocked");

        VERIFY_ARE_NOT_EQUAL(0, exitCode);
        VerifyPatternMatch(wsl::shared::string::WideToMultiByte(output), c_denialPatternExplicitAlpine);
    }

    // Positive path: build must proceed when FROM is on the allowlist.
    WSLC_TEST_METHOD(RegistryAllowlistAllowsImageBuild)
    {
        auto revert = SetRegistryAllowlist({L"mcr.microsoft.com"});

        auto [exitCode, output] =
            RunImageBuild(L"FROM mcr.microsoft.com/cbl-mariner/base/core:2.0\n", L"wsl-policy-build-allowed");

        if (exitCode != 0)
        {
            LogError("Expected build against allowlisted registry to succeed, got exit=%d output: '%ls'", exitCode, output.c_str());
            VERIFY_FAIL();
        }
    }

    // Case regression: allowlist entries stored uppercase must still match lowercased FROM.
    WSLC_TEST_METHOD(RegistryAllowlistImageBuildIsCaseInsensitive)
    {
        auto revert = SetRegistryAllowlist({L"MCR.MICROSOFT.COM"});

        auto [exitCode, output] = RunImageBuild(L"FROM mcr.microsoft.com/cbl-mariner/base/core:2.0\n", L"wsl-policy-build-case");

        if (exitCode != 0)
        {
            LogError("Expected uppercase allowlist entry to match lowercase FROM, got: '%ls'", output.c_str());
            VERIFY_FAIL();
        }
    }

    // Multi-stage regression: `COPY --from=<image>` must also be gated, not just top-level FROM.
    WSLC_TEST_METHOD(RegistryAllowlistBlocksImageBuildCopyFrom)
    {
        auto revert = SetRegistryAllowlist({L"mcr.microsoft.com"});
        const auto dockerfile =
            L"FROM mcr.microsoft.com/cbl-mariner/base/core:2.0\n"
            L"COPY --from=docker.io/library/alpine:latest /etc/os-release /tmp/os-release\n";

        auto [exitCode, output] = RunImageBuild(dockerfile, L"wsl-policy-build-copyfrom");

        VERIFY_ARE_NOT_EQUAL(0, exitCode);
        VerifyPatternMatch(wsl::shared::string::WideToMultiByte(output), c_denialPatternExplicitAlpine);
    }

    WSLC_TEST_METHOD(RegistryAllowlistBlocksImageBuildImplicitDockerIo)
    {
        auto revert = SetRegistryAllowlist({L"mcr.microsoft.com"});

        auto [exitCode, output] = RunImageBuild(L"FROM alpine:latest\n", L"wsl-policy-build-implicit");

        VERIFY_ARE_NOT_EQUAL(0, exitCode);
        VerifyPatternMatch(wsl::shared::string::WideToMultiByte(output), c_denialPatternImplicitAlpine);
    }

    // Runs `wslc image build` with the supplied Dockerfile content and returns the exit code
    // plus combined stdout/stderr. Extracted to keep the allowlist matrix above readable.
    static std::tuple<int, std::wstring> RunImageBuild(std::wstring_view dockerfile, std::wstring_view folder)
    {
        // Terminate any existing session so ConfigureBuildKitPolicy re-snapshots the registry.
        {
            std::wstring terminateCmd = L"\"" + GetWslcExePath() + L"\" system session terminate";
            LxsstuLaunchCommandAndCaptureOutputWithResult(terminateCmd.data(), nullptr, nullptr);
        }

        const auto contextDir = std::filesystem::temp_directory_path() / folder;
        std::error_code ec;
        std::filesystem::remove_all(contextDir, ec);
        std::filesystem::create_directories(contextDir);
        auto cleanup = wil::scope_exit([&] { std::filesystem::remove_all(contextDir, ec); });
        {
            std::ofstream df(contextDir / L"Dockerfile");
            VERIFY_IS_TRUE(df.is_open());
            df << wsl::shared::string::WideToMultiByte(std::wstring{dockerfile});
        }
        std::wstring cmd = L"\"" + GetWslcExePath() + L"\" image build \"" + contextDir.wstring() + L"\"";
        auto [stdoutText, stderrText, exitCode] = LxsstuLaunchCommandAndCaptureOutputWithResult(cmd.data(), nullptr, nullptr);
        return {exitCode, stdoutText + stderrText};
    }

    // Pure-function tests for the registry-allowlist policy evaluator. These don't talk to the
    // service, but do read/write the WSL policies registry key (created by TestClassSetup).
    TEST_METHOD(IsRegistryAllowed_Logic)
    {
        // No policy key configured -> always allowed.
        VERIFY_IS_TRUE(IsRegistryAllowed(nullptr, L"docker.io"));

        const auto policiesKey = OpenPoliciesKey();
        VERIFY_IS_TRUE(!!policiesKey);

        // No allowlist sub-key configured -> allowed.
        VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"docker.io"));

        // Allowlist with multiple entries; matching is case-insensitive.
        {
            auto revert = SetRegistryAllowlist({L"mcr.microsoft.com", L"Docker.IO"});
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"mcr.microsoft.com"));
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"docker.io"));
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"DOCKER.IO"));
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"MCR.Microsoft.COM"));
            VERIFY_IS_FALSE(IsRegistryAllowed(policiesKey.get(), L"ghcr.io"));
        }

        // Sub-key present with no entries -> no effective restriction, every server allowed.
        {
            auto revert = SetRegistryAllowlist({});
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"docker.io"));
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"mcr.microsoft.com"));
        }

        // Sub-key present but only contains empty entries -> treated as no restriction, not
        // as a deny-all (defensive against stray GP editor list items).
        {
            auto revert = SetRegistryAllowlist({L"", L""});
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"docker.io"));
            VERIFY_IS_TRUE(IsRegistryAllowed(policiesKey.get(), L"mcr.microsoft.com"));
        }
    }

    // Pure-function tests for HasRegistryAllowlist (used by `wslc image build` to decide whether
    // to refuse outright when the operation cannot be attributed to a single registry).
    TEST_METHOD(HasRegistryAllowlist_Logic)
    {
        VERIFY_IS_FALSE(HasRegistryAllowlist(nullptr));

        const auto policiesKey = OpenPoliciesKey();
        VERIFY_IS_TRUE(!!policiesKey);

        // No sub-key -> not configured.
        VERIFY_IS_FALSE(HasRegistryAllowlist(policiesKey.get()));

        // Sub-key present with no entries -> not effectively configured.
        {
            auto revert = SetRegistryAllowlist({});
            VERIFY_IS_FALSE(HasRegistryAllowlist(policiesKey.get()));
        }

        // Sub-key present with entries -> configured.
        {
            auto revert = SetRegistryAllowlist({L"mcr.microsoft.com"});
            VERIFY_IS_TRUE(HasRegistryAllowlist(policiesKey.get()));
        }
    }

    // The (HKEY) overload is exercised transitively via FromPoliciesRoot.
    TEST_METHOD(ReadRegistryAllowlistSnapshot_Logic)
    {
        // No sub-key -> NotConfigured.
        {
            const auto snapshot = ReadRegistryAllowlistSnapshotFromPoliciesRoot();
            VERIFY_IS_TRUE(snapshot.State == RegistryAllowlistState::NotConfigured);
            VERIFY_IS_TRUE(snapshot.Hosts.empty());
        }

        // Sub-key with only empty entries -> NotConfigured (defensive: stray blank GP list
        // items must not silently deny every registry).
        {
            auto revert = SetRegistryAllowlist({L"", L""});
            const auto snapshot = ReadRegistryAllowlistSnapshotFromPoliciesRoot();
            VERIFY_IS_TRUE(snapshot.State == RegistryAllowlistState::NotConfigured);
            VERIFY_IS_TRUE(snapshot.Hosts.empty());
        }

        // Sub-key with hosts -> Configured, hosts populated in order.
        {
            auto revert = SetRegistryAllowlist({L"mcr.microsoft.com", L"Docker.IO"});
            const auto snapshot = ReadRegistryAllowlistSnapshotFromPoliciesRoot();
            VERIFY_IS_TRUE(snapshot.State == RegistryAllowlistState::Configured);
            VERIFY_ARE_EQUAL(size_t{2}, snapshot.Hosts.size());
        }
    }
};
