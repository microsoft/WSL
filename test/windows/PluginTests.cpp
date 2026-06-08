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
#include "wslc.h"
#include "WSLCContainerLauncher.h"
#include "WSLCProcessLauncher.h"
#include "wslc/e2e/WSLCE2EHelpers.h"

using namespace wsl::windows::common::registry;
using WSLCE2ETests::StartLocalRegistry;

extern std::wstring g_testDistroPath;
extern std::wstring g_testDataPath;

class PluginTests
{
    std::wstring logFile;
    bool m_initialized = false;
    std::wstring pluginDll;
    std::optional<WslConfigChange> config;

    // Returns true if the file does not exist, cannot be stat'd, or is zero
    // bytes. Uses the non-throwing std::filesystem overloads so racy file
    // deletion / access denials between the existence check and size query
    // can't surface as exceptions out of test code.
    static bool LogFileAbsentOrEmpty(const std::filesystem::path& path)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        return static_cast<bool>(ec) || size == 0;
    }

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

    WSL2_TEST_METHOD(Success)
    {
        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=1
            VM created (settings->CustomConfigurationFlags=0)
            Folder mounted (* -> /test-plugin)
            Process created
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
            VM Stopping)";

        ConfigurePlugin(PluginTestType::Success);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(CustomKernelOverriddenByPolicy)
    {
        RegistryKeyChange policy(
            HKEY_LOCAL_MACHINE, wsl::windows::policies::c_registryKey, wsl::windows::policies::c_allowCustomKernelUserSetting, static_cast<DWORD>(0));

        WslConfigChange config(LxssGenerateTestConfig({.kernel = L"kernel-that-doesn't-exist"}));

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=1
            VM created (settings->CustomConfigurationFlags=0)
            Folder mounted (* -> /test-plugin)
            Process created
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
            VM Stopping)";

        ConfigurePlugin(PluginTestType::Success);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(DuplicatedPlugin)
    {
        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=1
            VM created (settings->CustomConfigurationFlags=0)
            Folder mounted (* -> /test-plugin)
            Process created
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
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

    WSL2_TEST_METHOD(CustomKernel)
    {
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

    WSL2_TEST_METHOD(CustomKernelCommandLine)
    {
        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=1
            VM created (settings->CustomConfigurationFlags=2)
            Folder mounted (* -> /test-plugin)
            Process created
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
            VM Stopping)";

        WslConfigChange config(LxssGenerateTestConfig({.vmIdleTimeout = 1, .kernelCommandLine = L"custom"}));

        ConfigurePlugin(PluginTestType::Success);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(DistroIdStaysTheSame)
    {
        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=10
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
            VM Stopping
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
            OnDistroStarted: received same GUID
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
            VM Stopping)";

        ConfigurePlugin(PluginTestType::SameDistroId);
        StartWsl(0);
        WslShutdown();
        StartWsl(0);

        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(InitPidIsDifferent)
    {
        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=14
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
            Distribution Stopping, name=test_distro, package=, PidNs=*
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
            Init's pid is different (* ! = *)
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
            VM Stopping)";

        ConfigurePlugin(PluginTestType::InitPidIsDifferent);
        StartWsl(0);
        TerminateDistribution();
        StartWsl(0);

        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(PluginUpdateRequired)
    {
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

    WSL2_TEST_METHOD(APIErrors)
    {
        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=7
            VM created (settings->CustomConfigurationFlags=0)
            API error tests passed
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
            VM Stopping)";

        ConfigurePlugin(PluginTestType::ApiErrors);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    WSL1_TEST_METHOD(SuccessWSL1)
    {
        // Plugins are not loaded for WSL1-only sessions (no VM, no plugin hooks).
        // Verify the plugin log file is absent/empty to assert no plugin code ran.
        ConfigurePlugin(PluginTestType::Success);
        StartWsl(0);

        VERIFY_IS_TRUE(
            LogFileAbsentOrEmpty(logFile), std::format(L"Expected plugin log file '{}' to be absent or empty for WSL1", logFile).c_str());
    }

    WSL2_TEST_METHOD(LoadFailureFatalWSL2)
    {
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

    WSL1_TEST_METHOD(LoadFailureNonFatalWSL1)
    {
        // Plugins are not loaded for WSL1-only sessions, so a plugin that
        // would fail to load on WSL2 has no effect on WSL1. Assert the plugin
        // log file is absent/empty to confirm no plugin code ran.
        ConfigurePlugin(PluginTestType::FailToLoad);
        StartWsl(0);

        VERIFY_IS_TRUE(
            LogFileAbsentOrEmpty(logFile), std::format(L"Expected plugin log file '{}' to be absent or empty for WSL1", logFile).c_str());
    }

    WSL2_TEST_METHOD(VmStartFailure)
    {
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

    WSL2_TEST_METHOD(VmStartFailureWithPluginErrorTwice)
    {
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

    WSL2_TEST_METHOD(VmStopFailure)
    {
        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=5
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
            VM Stopping
            OnVmStopping: E_UNEXPECTED)";

        ConfigurePlugin(PluginTestType::FailToStopVm);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(DistributionStartFailure)
    {
        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=4
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
            OnDistroStarted: E_UNEXPECTED
            VM Stopping)";

        constexpr auto ExpectedError =
            L"A fatal error was returned by plugin 'TestPlugin'\r\nError code: "
            L"Wsl/Service/CreateInstance/Plugin/E_UNEXPECTED\r\n";

        ConfigurePlugin(PluginTestType::FailToStartDistro);
        StartWsl(-1, ExpectedError);
        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(DistributionStopFailure)
    {
        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=6
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
            OnDistroStopping: E_UNEXPECTED
            VM Stopping)";

        ConfigurePlugin(PluginTestType::FailToStopDistro);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(ErrorMessageStartVm)
    {
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

    WSL2_TEST_METHOD(ErrorMessageStartDistro)
    {
        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=12
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
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

    WSL2_TEST_METHOD(RegisterSuccess)
    {
        ConfigurePlugin(PluginTestType::Success);

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--import plugin-test-distro . \"" + g_testDistroPath + L"\" --version 2"), 0L);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unregister plugin-test-distro"), 0L);

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=1
                VM created (settings->CustomConfigurationFlags=0)
                Folder mounted (* -> /test-plugin)
                Process created
                Distribution registered, name=plugin-test-distro, package=, Flavor=debian, Version=13
                Distribution unregistered, name=plugin-test-distro, package=, Flavor=debian, Version=13
                VM Stopping)";

        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(ImportInplaceSuccess)
    {
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
                Distribution registered, name=plugin-test-distro, package=, Flavor=debian, Version=13
                VM Stopping
                Distribution unregistered, name=plugin-test-distro, package=, Flavor=debian, Version=13
                VM created (settings->CustomConfigurationFlags=0)
                Folder mounted (* -> /test-plugin)
                Process created
                Distribution registered, name=plugin-test-distro-vhd, package=, Flavor=debian, Version=13
                Distribution unregistered, name=plugin-test-distro-vhd, package=, Flavor=debian, Version=13
                VM Stopping)";

        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(RegisterUnregisterFail)
    {
        ConfigurePlugin(PluginTestType::FailToRegisterUnregisterDistro);

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--import plugin-test-distro . \"" + g_testDistroPath + L"\" --version 2"), 0L);
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"--unregister plugin-test-distro"), 0L);

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=15
                VM created (settings->CustomConfigurationFlags=0)
                Distribution registered, name=plugin-test-distro, package=, Flavor=debian, Version=13
                OnDistributionRegistered: E_UNEXPECTED
                Distribution unregistered, name=plugin-test-distro, package=, Flavor=debian, Version=13
                OnDistributionUnregistered: E_UNEXPECTED
                VM Stopping)";

        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(ExecuteDistroCommand)
    {
        ConfigurePlugin(PluginTestType::RunDistroCommand);

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=16
                VM created (settings->CustomConfigurationFlags=0)
                Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
                Process created
                Failed process launch returned:  -2147467259
                Invalid distro launch returned:  -2147220717
                Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
                VM Stopping)";

        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(PluginToken)
    {
        ConfigurePlugin(PluginTestType::GetUsername);

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=17
                VM created (settings->CustomConfigurationFlags=0)
                Username: *
                Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
                Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
                VM Stopping)";

        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    static wil::com_ptr<IWSLCSessionManager> OpenWslcSessionManager()
    {
        wil::com_ptr<IWSLCSessionManager> sessionManager;
        VERIFY_SUCCEEDED(CoCreateInstance(__uuidof(WSLCSessionManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&sessionManager)));
        wsl::windows::common::security::ConfigureForCOMImpersonation(sessionManager.get());
        return sessionManager;
    }

    static wil::com_ptr<IWSLCSession> CreateWslcSession(LPCWSTR Name, WSLCNetworkingMode NetworkingMode = WSLCNetworkingModeNone)
    {
        WSLCSessionSettings settings{};
        settings.DisplayName = Name;
        settings.CpuCount = 4;
        settings.MemoryMb = 4096;
        settings.BootTimeoutMs = 30 * 1000;
        settings.NetworkingMode = NetworkingMode;

        auto manager = OpenWslcSessionManager();
        wil::com_ptr<IWSLCSession> session;
        VERIFY_SUCCEEDED(manager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session));
        wsl::windows::common::security::ConfigureForCOMImpersonation(session.get());

        WSLCSessionState state{};
        VERIFY_SUCCEEDED(session->GetState(&state));
        VERIFY_ARE_EQUAL(state, WSLCSessionStateRunning);

        return session;
    }

    WSL2_TEST_METHOD(WslcSuccess)
    {
        ConfigurePlugin(PluginTestType::WslcSuccess);

        {
            auto session = CreateWslcSession(L"plugin-wslc-test");

            LoadTestImage(*session, "debian:latest");

            // Create a container that will have a stuck process so it's still in a running state when the callback is made.
            wsl::windows::common::WSLCContainerLauncher launcher(
                "debian:latest", "wslc-plugin-container", {"/bin/sh", "-c", "sleep 120"});

            auto container = launcher.Launch(*session, WSLCContainerStartFlagsAttach);
            VERIFY_SUCCEEDED(container.Get().Stop(WSLCSignalSIGKILL, 0));

            // Delete the image so we get an ImageDeleted notification before the session goes away.
            WSLCDeleteImageOptions options{.Image = "debian:latest", .Flags = WSLCDeleteImageFlagsForce};
            wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deletedImages;
            VERIFY_SUCCEEDED(session->DeleteImage(&options, deletedImages.addressof(), deletedImages.size_address<ULONG>()));
        }

        const auto ExpectedOutput = std::format(
            LR"(Plugin loaded. TestMode=18
            WSLC Session created, name=plugin-wslc-test, id=*, pid=*, token=set, sid=set
            Command: 'echo -n stdout-ok && echo -n stderr-ok >&2', status=0, stdout: stdout-ok, stderr: stderr-ok
            Command: 'cat', status=0, stdout: stdin-ok, stderr: 
            Command: 'exit 12', status=12, stdout: , stderr: 
            Command: 'echo -n $ENV', status=0, stdout: env-ok, stderr: 
            WSLCCreateProcess(does-not-exist): {:x}, errno=2
            WSLCProcessGetFd(999): {}
            WSLCProcessGetExitCode(<running>): {}
            WSLC RW folder mounted at: /mnt/wsl-plugin/plugin-rw-test
            Command: 'cat /mnt/wsl-plugin/plugin-rw-test/plugin-test.txt', status=0, stdout: Windows-content, stderr: 
            WSLC RO folder mounted at: /mnt/wsl-plugin/plugin-ro-test
            Command: 'echo fail > /mnt/wsl-plugin/plugin-ro-test/should-not-exist.txt', status=1, stdout: , stderr: *
            WSLCMountFolder(nonexistent): {}
            WSLCMountFolder(relative): {}
            Test completed
            WSLC Container started, session=*, id=*, name=wslc-plugin-container, image=debian:latest, state=*
            WSLC Container stopping, session=*, id=*
            WSLC Image deleted, session=*, id=*
            WSLC Session stopping, name=plugin-wslc-test, id=*)",
            static_cast<uint32_t>(E_FAIL),
            E_INVALIDARG,
            HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
            HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND),
            E_INVALIDARG);

        ValidateLogFile(ExpectedOutput.c_str());
    }

    WSL2_TEST_METHOD(WslcPullImageNotification)
    {
        ConfigurePlugin(PluginTestType::WslcImagePull);

        {
            auto session = CreateWslcSession(L"plugin-wslc-pull-test", WSLCNetworkingModeVirtioProxy);

            // Load the registry and debian images.
            LoadTestImage(*session, "debian:latest");

            // Start a local registry container.
            auto [registryContainer, registryAddress] = StartLocalRegistry(*session);

            // Tag debian:latest for the local registry and push it.
            auto registryImage = std::format("{}/debian:latest", registryAddress);
            auto registryRepo = std::format("{}/debian", registryAddress);
            WSLCTagImageOptions tagOptions{};
            tagOptions.Image = "debian:latest";
            tagOptions.Repo = registryRepo.c_str();
            tagOptions.Tag = "latest";
            VERIFY_SUCCEEDED(session->TagImage(&tagOptions));

            auto emptyAuth = wsl::windows::common::wslutil::BuildRegistryAuthHeader("", "");
            VERIFY_SUCCEEDED(session->PushImage(registryImage.c_str(), emptyAuth.c_str(), nullptr, nullptr));

            // Delete the local tagged copy so PullImage actually downloads it.
            WSLCDeleteImageOptions deleteOpts{.Image = registryImage.c_str(), .Flags = WSLCDeleteImageFlagsNone};
            wil::unique_cotaskmem_array_ptr<WSLCDeletedImageInformation> deletedImages;
            VERIFY_SUCCEEDED(session->DeleteImage(&deleteOpts, deletedImages.addressof(), deletedImages.size_address<ULONG>()));

            // Pull the image back — this should trigger the ImageCreated plugin callback.
            VERIFY_SUCCEEDED(session->PullImage(registryImage.c_str(), nullptr, nullptr, nullptr));
        }

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=21
            WSLC Session created, name=plugin-wslc-pull-test, id=*, pid=*, token=set, sid=set
            WSLC Container started, session=*, id=*, name=*, image=wslc-registry:latest, state=running
            WSLC Image created, session=*, id=sha256:*, name=127.0.0.1:5000/debian:latest
            WSLC Session stopping, name=plugin-wslc-pull-test, id=*)";

        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(WslcSessionRejected)
    {
        ConfigurePlugin(PluginTestType::WslcSessionRejected);

        WSLCSessionSettings settings{};
        settings.DisplayName = L"plugin-wslc-rejected";
        settings.CpuCount = 4;
        settings.MemoryMb = 2048;
        settings.BootTimeoutMs = 30 * 1000;
        settings.MaximumStorageSizeMb = 1024 * 20;
        settings.NetworkingMode = WSLCNetworkingModeNone;

        auto manager = OpenWslcSessionManager();
        wil::com_ptr<IWSLCSession> session;
        const auto hr = manager->CreateSession(&settings, WSLCSessionFlagsNone, nullptr, &session);
        ValidateCOMErrorMessageContains(L"A fatal error was returned by plugin 'TestPlugin'");
        VERIFY_ARE_EQUAL(hr, HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=19
            WSLC Session created, name=plugin-wslc-rejected, id=*, pid=*, token=set, sid=set
            OnWslcSessionCreated: ERROR_ACCESS_DENIED
            WSLC Session stopping, name=plugin-wslc-rejected, id=*)";

        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(WslcContainerRejected)
    {
        ConfigurePlugin(PluginTestType::WslcContainerRejected);

        {
            auto session = CreateWslcSession(L"plugin-wslc-container-rejected");

            LoadTestImage(*session, "debian:latest");

            wsl::windows::common::WSLCContainerLauncher launcher(
                "debian:latest", "wslc-plugin-rejected-container", {"/bin/sh", "-c", "echo nope"});

            auto [hr, container] = launcher.LaunchNoThrow(*session, WSLCContainerStartFlagsAttach);
            ValidateCOMErrorMessageContains(L"A fatal error was returned by plugin 'TestPlugin'");
            VERIFY_ARE_EQUAL(hr, HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
        }

        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=20
            WSLC Session created, name=plugin-wslc-container-rejected, id=*, pid=*, token=set, sid=set
            WSLC Container started, session=*, id=*, name=*, image=debian:latest, state=*
            OnWslcContainerStarted: ERROR_ACCESS_DENIED
            WSLC Session stopping, name=plugin-wslc-container-rejected, id=*)";

        ValidateLogFile(ExpectedOutput);
    }

    // --- PR #40120 (out-of-process plugin host) coverage ---
    //
    // These tests validate the new isolation and locking behavior:
    //   * HostCrashIsFatal                — host process crash aborts the guarded operation (fatal).
    //   * ConcurrentCallbacks             — concurrent shared_lock readers on m_callbackLock.
    //   * AsyncApiCallFromWorker          — cross-apartment plugin API call from a non-hook thread.
    //   * CallbacksDuringTerminationDoNotCrash — exclusive m_callbackLock drains in-flight
    //                                        callbacks before m_utilityVm.reset().

    WSL2_TEST_METHOD(HostCrashIsFatal)
    {
        // A plugin host process crash during a veto hook (OnVmStarted) is fatal:
        // the guarded operation is aborted with a fatal plugin error rather than
        // silently continuing (matching the pre-refactor behavior where an
        // in-process plugin crash took down WSL). The exact HRESULT is whichever
        // RPC/CO_E_* code COM surfaces for the dead host, so assert on the
        // user-facing prefix rather than an exact error code.
        ConfigurePlugin(PluginTestType::HostCrash);

        constexpr auto fatalPrefix = L"A fatal error was returned by plugin 'TestPlugin'";

        auto [output, error] = LxsstuLaunchWslAndCaptureOutput(L"echo -n OK", -1);
        VERIFY_IS_TRUE(
            output.find(fatalPrefix) != std::wstring::npos, std::format(L"Expected a fatal plugin error, got: '{}'", output).c_str());

        // The crash latches a fatal plugin error, so a subsequent operation also
        // fails — the host is not re-activated for this service lifetime. Whether
        // it fails fast via the latch (service still up) or re-crashes after the
        // on-demand service idle-restarts and reloads the plugin, the user-facing
        // result is the same fatal plugin error.
        auto [output2, error2] = LxsstuLaunchWslAndCaptureOutput(L"echo -n OK", -1);
        VERIFY_IS_TRUE(
            output2.find(fatalPrefix) != std::wstring::npos,
            std::format(L"Expected a fatal plugin error on the second attempt, got: '{}'", output2).c_str());

        // Confirm the plugin actually ran up to the crash point.
        StopWslService();

        std::wifstream file(logFile);
        const auto fileContent = std::wstring{std::istreambuf_iterator<wchar_t>(file), {}};
        LogInfo("Logfile: %ls", fileContent.c_str());

        VERIFY_IS_TRUE(
            fileContent.find(L"Crashing host") != std::wstring::npos,
            std::format(L"Expected the plugin to reach the crash point, log: '{}'", fileContent).c_str());
    }

    WSL2_TEST_METHOD(ConcurrentCallbacks)
    {
        // The hook spawns 4 threads behind two gates: the first ensures all
        // workers are spawned, the second rendezvouses them at the callback
        // boundary so maxConcurrent deterministically reaches 4 (proving the
        // plugin issues 4 callbacks concurrently). Per-thread logging is
        // intentionally suppressed; only the deterministic summary line is
        // asserted. Lifecycle pre/post lines validate the hook itself ran
        // to completion.
        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=23
            VM created (settings->CustomConfigurationFlags=0)
            Concurrent callbacks complete: success=4 failures=0 maxConcurrent=4
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
            VM Stopping)";

        ConfigurePlugin(PluginTestType::ConcurrentApiCalls);
        StartWsl(0);
        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(AsyncApiCallFromWorker)
    {
        // The worker thread is created in OnDistroStarted, sleeps briefly to
        // ensure it runs after the hook has returned, then calls
        // ExecuteBinaryInDistribution. It's joined unconditionally in
        // OnDistroStopping (which defers its own "Distribution Stopping" log
        // until after the join), so the worker-output line is guaranteed to
        // precede "Distribution Stopping" in the log.
        //
        // The wsl command sleeps for 1s so the distro is alive long enough
        // for the post-hook worker call to land before shutdown.
        constexpr auto ExpectedOutput =
            LR"(Plugin loaded. TestMode=24
            VM created (settings->CustomConfigurationFlags=0)
            Distribution started, name=test_distro, package=, PidNs=*, InitPid=*, Flavor=debian, Version=13
            Async worker output: hello-from-worker
            Distribution Stopping, name=test_distro, package=, PidNs=*, Flavor=debian, Version=13
            VM Stopping)";

        ConfigurePlugin(PluginTestType::AsyncApiCall);

        auto [output, error] = LxsstuLaunchWslAndCaptureOutput(L"sh -c \"sleep 1; echo -n OK\"", 0);
        VERIFY_ARE_EQUAL(output, L"OK");

        ValidateLogFile(ExpectedOutput);
    }

    WSL2_TEST_METHOD(CallbacksDuringTerminationDoNotCrash)
    {
        // Drain test: 4 workers loop ExecuteBinaryInDistribution (with /bin/true,
        // sub-ms callback) while the distro is alive. They keep calling across
        // OnDistroStopping and _VmTerminate; the exclusive m_callbackLock acquire
        // in _VmTerminate must drain in-flight callbacks before resetting
        // m_utilityVm. After OnVmStopping signals wind-down, workers run a bounded
        // number of further iterations and exit (see Plugin.cpp), so termination
        // is deterministic and no worker can revive against a later VM.
        //
        // The post-shutdown StartWsl below triggers a second OnDistroStarted that
        // joins the finished workers, so this test needs no fixed sleep.
        //
        // Scope:
        //   - Validates: dual-lock invariant under racing callbacks; drain works
        //     when callbacks complete in sub-ms; service survives the race.
        //   - Does NOT validate: drain semantics when a callback is genuinely
        //     stuck (e.g. service-side CreateLinuxProcess waiting on a hung
        //     Linux init). That requires cancellation plumbing through
        //     WslCoreInstance::CreateLinuxProcess and is tracked separately.
        ConfigurePlugin(PluginTestType::CallbackDuringTermination);

        // Use a 1s sleep so workers ramp up while the distro is alive.
        auto [output, error] = LxsstuLaunchWslAndCaptureOutput(L"sh -c \"sleep 1; echo -n OK\"", 0);
        VERIFY_ARE_EQUAL(output, L"OK");

        const DWORD pidBefore = GetWslServiceRunningPid();
        VERIFY_IS_TRUE(pidBefore != 0);

        // Trigger VM termination with workers still running.
        WslShutdown();

        // Subsequent WSL command must still succeed — service survived. Its
        // OnDistroStarted also joins the wound-down workers.
        StartWsl(0);

        const DWORD pidAfter = GetWslServiceRunningPid();
        VERIFY_ARE_EQUAL(pidBefore, pidAfter);
    }

    // This test must run last so it doesn't break test cases that depends on plugin signature.
    WSL2_TEST_METHOD(InvalidPluginSignature)
    {
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
