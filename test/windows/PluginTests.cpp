/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    PluginTests.cpp

Abstract:

    This file contains test cases for the plugin API.

--*/

#include "precomp.h"
#include "Common.h"
#include "registry.hpp"
#include "PluginTests.h"

using namespace wsl::windows::common::registry;

extern std::wstring g_testDistroPath;

class PluginTests
{
    std::wstring logFile;
    bool m_initialized = false;
    std::wstring pluginDll;
    std::optional<WslConfigChange> config;

    WSL_TEST_CLASS(PluginTests)

    TEST_CLASS_SETUP(TestClassSetup)
    {
        VERIFY_ARE_EQUAL(LxsstuInitialize(FALSE), TRUE);
        m_initialized = true;
        logFile = wil::GetCurrentDirectoryW<std::wstring>() + L"\\plugin-logs.txt";

        const auto currentDll = std::filesystem::path(wil::GetModuleFileNameW<std::wstring>(wil::GetModuleInstanceHandle()));
        const auto pluginDllPath = currentDll.parent_path() / L"testplugin.dll";
        if (!std::filesystem::exists(pluginDllPath))
        {
            const std::wstring message = L"Plugin not found in: " + pluginDllPath.wstring();
            VERIFY_FAIL(message.c_str());
            return false;
        }

        pluginDll = pluginDllPath.wstring();

        // Disable VM timeouts during the plugin tests
        config.emplace(LxssGenerateTestConfig({.vmIdleTimeout = -1}));

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {

        auto key = OpenLxssMachineKey(KEY_ALL_ACCESS);
        DeleteKey(key.get(), L"Test");

        key = OpenKey(key.get(), L"Plugins", KEY_SET_VALUE);
        DeleteValue(key.get(), L"TestPlugin");

        RestartWslService();

        std::wifstream file(logFile);
        const auto fileContent = std::wstring{std::istreambuf_iterator<wchar_t>(file), {}};
        LogInfo("Logfile: %ls", fileContent.c_str());
        file.close();

        StopWslService();
        if (!DeleteFile(logFile.c_str()))
        {
            VERIFY_ARE_EQUAL(ERROR_FILE_NOT_FOUND, GetLastError());
        }

        if (m_initialized)
        {
            LxsstuUninitialize(FALSE);
        }

        return true;
    }

    void ConfigurePlugin(PluginTestType testCase) const
    {
        StopWslService();
        if (!DeleteFile(logFile.c_str()))
        {
            VERIFY_ARE_EQUAL(ERROR_FILE_NOT_FOUND, GetLastError());
        }

        const auto testKey = OpenTestRegistryKey(KEY_SET_VALUE);
        WriteDword(testKey.get(), nullptr, c_testType, static_cast<DWORD>(testCase));
        WriteString(testKey.get(), nullptr, c_logFile, logFile.c_str());

        const auto lxssKey =
            CreateKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss\\Plugins", KEY_SET_VALUE, nullptr, 0);
        WriteString(lxssKey.get(), nullptr, L"TestPlugin", pluginDll.c_str());

        RestartWslService();
    }

    static void StartWsl(int expectedExitCode, LPCWSTR ExpectedOutput = nullptr)
    {
        auto [output, error] = LxsstuLaunchWslAndCaptureOutput(L"echo -n OK", expectedExitCode);
        if (expectedExitCode == 0)
        {
            VERIFY_ARE_EQUAL(output, L"OK");
        }
        else
        {
            VERIFY_ARE_EQUAL(output, ExpectedOutput);
        }
    }

    void ValidateLogFile(LPCWSTR expected) const
    {
        StopWslService();

        std::wifstream file(logFile);
        auto fileContent = std::wstring{std::istreambuf_iterator<wchar_t>(file), {}};
        LogInfo("Logfile: %ls", fileContent.c_str());

        auto fileLines = wsl::shared::string::Split<wchar_t>(fileContent, '\n');
        auto expectedLines = wsl::shared::string::Split<wchar_t>(expected, '\n');

        for (size_t i = 0; i < std::max(fileLines.size(), expectedLines.size()); i++)
        {
            if (i >= fileLines.size())
            {
                std::wstring message = L"Line is expected but not in log file: " + expectedLines[i];
                VERIFY_FAIL(message.c_str());
            }
            else if (i >= expectedLines.size())
            {
                std::wstring message = L"Line is in file but not expected: " + fileLines[i];
                VERIFY_FAIL(message.c_str());
            }

            const auto& expected = expectedLines[i];
            const auto& actual = fileLines[i];
            if (!PathMatchSpec(actual.c_str(), expected.c_str()))
            {
                LogInfo("Plugin log: %ls", fileContent.c_str());
                std::wstring message = L"Line (" + actual + L") didn't match pattern: " + expected;
                VERIFY_FAIL(message.c_str());
            }
        }
    }

    TEST_METHOD(Success)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=1
            VM created (settings->CustomConfigurationFlags=0)
            Folder mounted (* -> /test-plugin)
            Process created
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=12
            VM Stopping)";

        ConfigurePlugin(PluginTestType::Success);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(CustomKernelOverriddenByPolicy)
    {
        WSL2_TEST_ONLY();

        RegistryKeyChange policy(
            HKEY_LOCAL_MACHINE, wsl::windows::policies::c_registryKey, wsl::windows::policies::c_allowCustomKernelUserSetting, static_cast<DWORD>(0));

        WslConfigChange config(LxssGenerateTestConfig({.kernel = L"kernel-that-doesn't-exist"}));

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=1
            VM created (settings->CustomConfigurationFlags=0)
            Folder mounted (* -> /test-plugin)
            Process created
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=12
            VM Stopping)";

        ConfigurePlugin(PluginTestType::Success);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(DuplicatedPlugin)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=1
            VM created (settings->CustomConfigurationFlags=0)
            Folder mounted (* -> /test-plugin)
            Process created
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=12
            VM Stopping)";

        ConfigurePlugin(PluginTestType::Success);

        // Register the same plugin dll twice. Validate that it's only called once.
        const auto key =
            CreateKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss\\Plugins", KEY_SET_VALUE, nullptr, 0);
        WriteString(key.get(), nullptr, L"TestPlugin-duplicated", pluginDll.c_str());
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { DeleteValue(key.get(), L"TestPlugin-duplicated"); });
        RestartWslService();

        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(CustomKernel)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=1
            VM created (settings->CustomConfigurationFlags=1)
            OnVmStarted: E_ACCESSDENIED
            VM Stopping)";

#ifdef WSL_KERNEL_PATH

        std::wstring kernelPath = WIDEN(WSL_KERNEL_PATH);

#else

        auto kernelPath = wsl::windows::common::wslutil::GetMsiPackagePath().value_or(L"");
        VERIFY_IS_FALSE(kernelPath.empty());
        kernelPath += L"\\tools\\kernel";

#endif

        WslConfigChange config(LxssGenerateTestConfig({.vmIdleTimeout = 1, .kernel = kernelPath}));

        ConfigurePlugin(PluginTestType::Success);
        StartWsl(
            -1,
            L"A fatal error was returned by plugin 'TestPlugin'\r\nError code: "
            L"Wsl/Service/CreateInstance/CreateVm/Plugin/E_ACCESSDENIED\r\n");

        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(CustomKernelCommandLine)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=1
            VM created (settings->CustomConfigurationFlags=2)
            Folder mounted (* -> /test-plugin)
            Process created
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=12
            VM Stopping)";

        WslConfigChange config(LxssGenerateTestConfig({.vmIdleTimeout = 1, .kernelCommandLine = L"custom"}));

        ConfigurePlugin(PluginTestType::Success);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(DistroIdStaysTheSame)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=10
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=12
            VM Stopping
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
            OnDistroStarted: received same GUID
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=12
            VM Stopping)";

        ConfigurePlugin(PluginTestType::SameDistroId);
        StartWsl(0);
        WslShutdown();
        StartWsl(0);

        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(InitPidIsDifferent)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=14
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
            Distribution Stopping, name=test_distro, package=, PidNs=*
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
            Init's pid is different (* ! = *)
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=12
            VM Stopping)";

        ConfigurePlugin(PluginTestType::InitPidIsDifferent);
        StartWsl(0);
        TerminateDistribution();
        StartWsl(0);

        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(PluginUpdateRequired)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=9
            OnLoad: WSL_E_PLUGINREQUIRESUPDATE)";

        ConfigurePlugin(PluginTestType::PluginRequiresUpdate);
        StartWsl(
            -1,
            L"The plugin 'TestPlugin' requires a newer version of WSL. Please run: wsl.exe --update\r\nError code: "
            L"Wsl/Service/CreateInstance/CreateVm/Plugin/WSL_E_PLUGIN_REQUIRES_UPDATE\r\n");

        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(APIErrors)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=7
            VM created (settings->CustomConfigurationFlags=0)
            API error tests passed
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=12
            VM Stopping)";

        ConfigurePlugin(PluginTestType::ApiErrors);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(SuccessWSL1)
    {
        WSL1_TEST_ONLY();

        constexpr auto ExpectedOutput = LR"(Plugin loaded. TestMode=1)";

        ConfigurePlugin(PluginTestType::Success);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(LoadFailureFatalWSL2)
    {
        WSL2_TEST_ONLY();
        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=2
            OnLoad: E_UNEXPECTED)";

        ConfigurePlugin(PluginTestType::FailToLoad);
        StartWsl(
            -1,
            L"A fatal error was returned by plugin 'TestPlugin'\r\nError code: "
            L"Wsl/Service/CreateInstance/CreateVm/Plugin/E_UNEXPECTED\r\n");
        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(LoadFailureNonFatalWSL1)
    {
        WSL1_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=2
            OnLoad: E_UNEXPECTED)";

        ConfigurePlugin(PluginTestType::FailToLoad);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(VmStartFailure)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=3
            VM created (settings->CustomConfigurationFlags=0)
            OnVmStarted: E_UNEXPECTED
            VM Stopping)";

        ConfigurePlugin(PluginTestType::FailToStartVm);
        StartWsl(
            -1,
            L"A fatal error was returned by plugin 'TestPlugin'\r\nError code: "
            L"Wsl/Service/CreateInstance/CreateVm/Plugin/E_UNEXPECTED\r\n");
        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(VmStartFailureWithPluginErrorTwice)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=13
            VM created (settings->CustomConfigurationFlags=0)
            OnVmStarted: E_UNEXPECTED
            VM Stopping
            VM created (settings->CustomConfigurationFlags=0)
            OnVmStarted: E_UNEXPECTED
            VM Stopping)";

        ConfigurePlugin(PluginTestType::FailToStartVmWithPluginErrorMessage);

        StartWsl(
            -1,
            L"A fatal error was returned by plugin 'TestPlugin'. Error message: 'Plugin error message'\r\nError code: "
            L"Wsl/Service/CreateInstance/CreateVm/Plugin/E_UNEXPECTED\r\n");

        StartWsl(
            -1,
            L"A fatal error was returned by plugin 'TestPlugin'. Error message: 'Plugin error message'\r\nError code: "
            L"Wsl/Service/CreateInstance/CreateVm/Plugin/E_UNEXPECTED\r\n");

        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(VmStopFailure)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=5
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, Flavor=debian, Version=12
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=12
            VM Stopping
            OnVmStopping: E_UNEXPECTED)";

        ConfigurePlugin(PluginTestType::FailToStopVm);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(DistributionStartFailure)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=4
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
            OnDistroStarted: E_UNEXPECTED
            VM Stopping)";

        constexpr auto ExpectedError =
            L"A fatal error was returned by plugin 'TestPlugin'\r\nError code: "
            L"Wsl/Service/CreateInstance/Plugin/E_UNEXPECTED\r\n";

        ConfigurePlugin(PluginTestType::FailToStartDistro);
        StartWsl(-1, ExpectedError);
        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(DistributionStopFailure)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=6
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=12
            OnDistroStopping: E_UNEXPECTED
            VM Stopping)";

        ConfigurePlugin(PluginTestType::FailToStopDistro);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(ErrorMessageStartVm)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=11
            VM created (settings->CustomConfigurationFlags=0)
            OnVmStarted: E_FAIL
            VM Stopping)";

        ConfigurePlugin(PluginTestType::ErrorMessageStartVm);
        StartWsl(
            -1,
            L"A fatal error was returned by plugin 'TestPlugin'. Error message: 'StartVm plugin error message'\r\nError code: "
            L"Wsl/Service/CreateInstance/CreateVm/Plugin/E_FAIL\r\n");

        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(ErrorMessageStartDistro)
    {
        WSL2_TEST_ONLY();

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=12
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
            OnDistroStarted: E_FAIL
            VM Stopping)";

        ConfigurePlugin(PluginTestType::ErrorMessageStartDistro);
        StartWsl(
            -1,
            L"A fatal error was returned by plugin 'TestPlugin'. Error message: 'StartDistro plugin error message'\r\nError "
            L"code: "
            L"Wsl/Service/CreateInstance/Plugin/E_FAIL\r\n");

        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(RegisterSuccess)
    {
        WSL2_TEST_ONLY();

        ConfigurePlugin(PluginTestType::Success);

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--import plugin-test-distro . \"" + g_testDistroPath + L"\" --version 2"), 0L);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unregister plugin-test-distro"), 0L);

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=1
                VM created (settings->CustomConfigurationFlags=0)
                Folder mounted (* -> /test-plugin)
                Process created
                Distribution registered, name=plugin-test-distro, package=, Flavor=debian, Version=12
                Distribution unregistered, name=plugin-test-distro, package=, Flavor=debian, Version=12
                VM Stopping)";

        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(ImportInplaceSuccess)
    {
        WSL2_TEST_ONLY();

        ConfigurePlugin(PluginTestType::Success);

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--import plugin-test-distro . \"" + g_testDistroPath + L"\" --version 2"), 0L);
        WslShutdown();
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--export plugin-test-distro plugin-test-distro.vhdx --format vhd"), 0L);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unregister plugin-test-distro"), 0L);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--import-in-place plugin-test-distro-vhd plugin-test-distro.vhdx"), 0L);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unregister plugin-test-distro-vhd"), 0L);

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=1
                VM created (settings->CustomConfigurationFlags=0)
                Folder mounted (* -> /test-plugin)
                Process created
                Distribution registered, name=plugin-test-distro, package=, Flavor=debian, Version=12
                VM Stopping
                Distribution unregistered, name=plugin-test-distro, package=, Flavor=debian, Version=12
                VM created (settings->CustomConfigurationFlags=0)
                Folder mounted (* -> /test-plugin)
                Process created
                Distribution registered, name=plugin-test-distro-vhd, package=, Flavor=debian, Version=12
                Distribution unregistered, name=plugin-test-distro-vhd, package=, Flavor=debian, Version=12
                VM Stopping)";

        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(RegisterUnregisterFail)
    {
        WSL2_TEST_ONLY();

        ConfigurePlugin(PluginTestType::FailToRegisterUnregisterDistro);

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--import plugin-test-distro . \"" + g_testDistroPath + L"\" --version 2"), 0L);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unregister plugin-test-distro"), 0L);

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=15
                VM created (settings->CustomConfigurationFlags=0)
                Distribution registered, name=plugin-test-distro, package=, Flavor=debian, Version=12
                OnDistributionRegistered: E_UNEXPECTED
                Distribution unregistered, name=plugin-test-distro, package=, Flavor=debian, Version=12
                OnDistributionUnregistered: E_UNEXPECTED
                VM Stopping)";

        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(ExecuteDistroCommand)
    {
        WSL2_TEST_ONLY();

        ConfigurePlugin(PluginTestType::RunDistroCommand);

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=16
                VM created (settings->CustomConfigurationFlags=0)
                Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
                Process created
                Failed process launch returned:  -2147467259
                Invalid distro launch returned:  -2147220717
                Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=12
                VM Stopping)";

        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    TEST_METHOD(PluginToken)
    {
        WSL2_TEST_ONLY();

        ConfigurePlugin(PluginTestType::GetUsername);

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=17
                VM created (settings->CustomConfigurationFlags=0)
                Username: *
                Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=12
                Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=12
                VM Stopping)";

        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }
    // This test must run last so it doesn't break test cases that depends on plugin signature.
    TEST_METHOD(InvalidPluginSignature)
    {
        WSL2_TEST_ONLY();

        if constexpr (!wsl::shared::OfficialBuild)
        {
            LogSkipped("This test only applies to signed builds");
            return;
        }

        StopWslService();

        // Append one byte at the end of the plugin dll to break its signature
        wil::unique_handle plugin{CreateFile(pluginDll.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr)};
        VERIFY_IS_TRUE(plugin.is_valid());

        char c{};
        VERIFY_IS_TRUE(WriteFile(plugin.get(), &c, sizeof(c), nullptr, nullptr));
        plugin.reset();

        ConfigurePlugin(PluginTestType::ErrorMessageStartDistro);
        StartWsl(
            -1,
            L"A fatal error was returned by plugin 'TestPlugin'\r\nError code: "
            L"Wsl/Service/CreateInstance/CreateVm/Plugin/TRUST_E_NOSIGNATURE\r\n");
    }
};