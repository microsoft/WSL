/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PolicyTests.cpp

Abstract:

    This file contains test cases for WSL policies.

--*/

#include "precomp.h"
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

    TEST_METHOD(MountPolicyAllowed)
    {
        SKIP_TEST_ARM64();
        WSL2_TEST_ONLY();

        auto revert = SetPolicy(c_allowDiskMount, 1);
        ValidateOutput(
            L"--mount DoesNotExist",
            L"Failed to attach disk 'DoesNotExist' to WSL2: The system cannot find the file specified. \r\n"
            L"Error code: Wsl/Service/AttachDisk/MountDisk/HCS/ERROR_FILE_NOT_FOUND\r\n");
    }

    TEST_METHOD(MountPolicyDisabled)
    {
        SKIP_TEST_ARM64();
        WSL2_TEST_ONLY();

        auto revert = SetPolicy(c_allowDiskMount, 0);
        ValidateOutput(
            L"--mount DoesNotExist",
            L"wsl.exe --mount is disabled by the computer policy.\r\nError code: Wsl/Service/WSL_E_DISK_MOUNT_DISABLED\r\n");
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

    TEST_METHOD(KernelCommandLine)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(NestedVirtualization)
    {
        SKIP_TEST_ARM64();
        WSL2_TEST_ONLY();
        WINDOWS_11_TEST_ONLY();

        ValidatePolicy(
            c_allowNestedVirtualizationUserSetting,
            L"nestedVirtualization=true",
            L"wsl: The .wslconfig setting 'wsl2.nestedVirtualization' is disabled by the computer policy.\r\n");
    }

    TEST_METHOD(KernelDebugging)
    {
        WSL2_TEST_ONLY();

        WINDOWS_11_TEST_ONLY();

        ValidatePolicy(
            c_allowKernelDebuggingUserSetting,
            L"kernelDebugPort=1234",
            L"wsl: The .wslconfig setting 'wsl2.kernelDebugPort' is disabled by the computer policy.\r\n");
    }

    TEST_METHOD(CustomKernel)
    {
        WSL2_TEST_ONLY();

        const std::wstring wslConfigPath = wsl::windows::common::helpers::GetWslConfigPath();
        const std::wstring nonExistentFile = L"DoesNotExist";
        WslConfigChange config(LxssGenerateTestConfig({.kernel = nonExistentFile.c_str(), .kernelModules = nonExistentFile.c_str()}));

        {
            auto revert = SetPolicy(c_allowCustomKernelUserSetting, 1);
            WslShutdown();

            ValidateOutput(
                L"echo ok",
                std::format(
                    L"{}\r\nError code: Wsl/Service/CreateInstance/CreateVm/WSL_E_CUSTOM_KERNEL_NOT_FOUND\r\n",
                    wsl::shared::Localization::MessageCustomKernelNotFound(wslConfigPath, nonExistentFile)));
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

    TEST_METHOD(CustomSystemDistro)
    {
        WSL2_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig() + L"systemDistro=DoesNotExist");
        const std::wstring wslConfigPath = wsl::windows::common::helpers::GetWslConfigPath();

        {
            auto revert = SetPolicy(c_allowCustomSystemDistroUserSetting, 1);
            WslShutdown();

            ValidateOutput(
                L"echo ok",
                L"The custom system distribution specified in " + wslConfigPath +
                    L" was not found or is not the correct format.\r\nError code: "
                    L"Wsl/Service/CreateInstance/CreateVm/WSL_E_CUSTOM_SYSTEM_DISTRO_ERROR\r\n");
        }

        {
            auto revert = SetPolicy(c_allowCustomSystemDistroUserSetting, 0);
            WslShutdown();

            ValidateWarnings(L"wsl: The .wslconfig setting 'wsl2.systemDistro' is disabled by the computer policy.\r\n");
        }
    }

    TEST_METHOD(CustomNetworkingMode)
    {
        WSL2_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::VirtioProxy}));

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
            auto revertDefault = SetPolicy(c_defaultNetworkingMode, static_cast<DWORD>(wsl::core::NetworkingMode::VirtioProxy));
            config.Update(LxssGenerateTestConfig({.networkingMode = wsl::core::NetworkingMode::VirtioProxy}));
            ValidateWarnings(L"");
        }
    }

    TEST_METHOD(DebugShell)
    {
        WSL2_TEST_ONLY();

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
                    L"WSL1 is disabled by the computer policy.\r\nError code: Wsl/Service/WSL_E_WSL1_DISABLED\r\n");
            }
            else
            {
                ValidateOutput(
                L"echo ok",
                L"WSL1 is disabled by the computer policy.\r\nPlease run 'wsl.exe --set-version " LXSS_DISTRO_NAME_TEST_L L" 2' to upgrade to WSL2.\r\nError code: Wsl/Service/CreateInstance/WSL_E_WSL1_DISABLED\r\n");
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
                    L"This program is blocked by group policy. For more information, contact your system administrator. "
                    L"\r\nError "
                    L"code: Wsl/ERROR_ACCESS_DISABLED_BY_POLICY\r\n");
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

    TEST_METHOD(DefaultNetworkingMode)
    {
        WSL2_TEST_ONLY();

        WslConfigChange config(LxssGenerateTestConfig());

        {
            auto revert = SetPolicy(c_defaultNetworkingMode, static_cast<DWORD>(wsl::core::NetworkingMode::None));
            WslShutdown();

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"wslinfo --networking-mode | grep -iF 'none'"), 0u);
        }

        {
            auto revert = SetPolicy(c_defaultNetworkingMode, static_cast<DWORD>(wsl::core::NetworkingMode::VirtioProxy));
            WslShutdown();

            VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"wslinfo --networking-mode | grep -iF 'virtioproxy'"), 0u);
        }
    }
};