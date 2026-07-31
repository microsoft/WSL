/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SimpleTests.cpp

Abstract:

    This file contains smoke tests for WSL.

--*/

#include "precomp.h"
#include "Common.h"
#include "SubProcess.h"

namespace SimpleTests {
class SimpleTests
{
    WSL_TEST_CLASS(SimpleTests)

    // Initialize the tests
    TEST_CLASS_SETUP(TestClassSetup)
    {
        VERIFY_ARE_EQUAL(LxsstuInitialize(FALSE), TRUE);

        return true;
    }

    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        LxsstuUninitialize(FALSE);
        return true;
    }

    TEST_METHOD(EchoTest)
    {
        const std::wstring echoExpected = L"LOW!\n";
        auto [output, __] = LxsstuLaunchWslAndCaptureOutput(L"echo LOW!");
        VERIFY_ARE_EQUAL(output, echoExpected);
    }

    TEST_METHOD(WhoamiTest)
    {
        const std::wstring whoamiExpected = L"root\n";
        auto [output, __] = LxsstuLaunchWslAndCaptureOutput(L"-u root whoami");
        VERIFY_ARE_EQUAL(output, whoamiExpected);
    }

    TEST_METHOD(ChangeDirTest)
    {
        const std::wstring cdExpected = L"/root\n";
        auto [output, __] = LxsstuLaunchWslAndCaptureOutput(L"--cd ~ --user root pwd");
        VERIFY_ARE_EQUAL(output, cdExpected);
    }

    TEST_METHOD(Daemonize)
    {
        WslConfigChange config(LxssGenerateTestConfig({.vmIdleTimeout = 0}));
        WslShutdown();
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"-- eval \"touch /dev/shm/backgroundmagic; daemonize $(which sleep) 30\""), (DWORD)0);

        std::this_thread::sleep_for(std::chrono::seconds(20));

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"-- ls /dev/shm/backgroundmagic"), (DWORD)0);
    }

    static void VerifySparse(wchar_t const* path, bool sparse)
    {
        DWORD attributes = ::GetFileAttributesW(path);
        VERIFY_IS_FALSE(attributes == INVALID_FILE_ATTRIBUTES);
        VERIFY_IS_TRUE(WI_IsFlagSet(attributes, FILE_ATTRIBUTE_SPARSE_FILE) == sparse);
    }

    WSL2_TEST_METHOD(CheckSparse)
    {
        WslConfigChange config(LxssGenerateTestConfig({.sparse = true}));

        std::filesystem::path tar = std::tmpnam(nullptr);
        tar += ".tar";
        LogInfo("tar %ls", tar.c_str());
        auto cleanupTar = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            try
            {
                std::filesystem::remove(tar);
            }
            CATCH_LOG()
        });

        const std::wstring tempDistro = L"temp_distro";
        const std::filesystem::path vhdDir = std::tmpnam(nullptr);
        LogInfo("vhdDir %ls", vhdDir.c_str());
        VERIFY_IS_TRUE(std::filesystem::create_directory(vhdDir));
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&] {
            try
            {
                LxsstuLaunchWsl(std::format(L"{} {}", WSL_UNREGISTER_ARG, tempDistro).c_str());
                std::filesystem::remove_all(vhdDir);
            }
            CATCH_LOG()
        });

        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(std::format(L"{} {} {}", WSL_EXPORT_ARG, LXSS_DISTRO_NAME_TEST, tar.wstring()).c_str()), (DWORD)0);
        LxsstuLaunchWsl(std::format(L"{} {}", WSL_UNREGISTER_ARG, tempDistro).c_str());
        ValidateOutput(
            std::format(L"{} {} {} {}", WSL_IMPORT_ARG, tempDistro, vhdDir.wstring(), tar.wstring()).c_str(),
            L"The operation completed successfully. \r\n",
            L"wsl: Sparse VHD support is currently disabled due to potential data corruption.\r\n"
            L"To force a distribution to use a sparse VHD, please run:\r\n"
            L"wsl.exe --manage <DistributionName> --set-sparse true --allow-unsafe\r\n",
            0);

        std::filesystem::path vhdPath = vhdDir / LXSS_VM_MODE_VHD_NAME;
        VerifySparse(vhdPath.c_str(), false);

        WslShutdown();

        // Setting a distro VHD to sparse requires the allow unsafe flag.
        ValidateOutput(
            std::format(L"{} {} {} {}", WSL_MANAGE_ARG, tempDistro, WSL_MANAGE_ARG_SET_SPARSE_OPTION_LONG, L"true").c_str(),
            L"Sparse VHD support is currently disabled due to potential data corruption.\r\n"
            L"To force a distribution to use a sparse VHD, please run:\r\n"
            L"wsl.exe --manage <DistributionName> --set-sparse true --allow-unsafe\r\nError code: Wsl/Service/E_INVALIDARG\r\n",
            L"",
            -1);

        VerifySparse(vhdPath.c_str(), false);

        ValidateOutput(
            std::format(L"{} {} {} {} {}", WSL_MANAGE_ARG, tempDistro, WSL_MANAGE_ARG_SET_SPARSE_OPTION_LONG, L"true", WSL_MANAGE_ARG_ALLOW_UNSAFE)
                .c_str(),
            L"The operation completed successfully. \r\n",
            L"",
            0);

        VerifySparse(vhdPath.c_str(), true);

        // Disabling sparse on a VHD does not require the allow unsafe flag.
        ValidateOutput(
            std::format(L"{} {} {} {}", WSL_MANAGE_ARG, tempDistro, WSL_MANAGE_ARG_SET_SPARSE_OPTION_LONG, L"false").c_str(),
            L"The operation completed successfully. \r\n",
            L"",
            0);

        VerifySparse(vhdPath.c_str(), false);
    }

    static std::wstring BuildControllableWslCommandLine()
    {
        // The child prints "ready" as soon as the Linux process is running,
        // then blocks on stdin so the parent can deterministically control its lifetime.
        return LxssGenerateWslCommandLine(L"-- sh -c \"printf ready; IFS= read -r _\"");
    }

    static DWORD WaitForProcessExit(HANDLE process, DWORD timeoutMs)
    {
        return wsl::windows::common::SubProcess::GetExitCode(process, timeoutMs);
    }

    static wil::unique_handle StartControllableWslProcess(HANDLE standardInput, HANDLE standardOutput, DWORD createFlags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT)
    {
        std::wstring commandLine = BuildControllableWslCommandLine();
        return LxsstuStartProcess(commandLine.data(), standardInput, standardOutput, standardOutput, nullptr, createFlags);
    }

    struct unique_kill_process
    {
        unique_kill_process() = default;

        explicit unique_kill_process(wil::unique_handle&& process) : m_process(std::move(process))
        {
        }

        unique_kill_process(unique_kill_process&&) = default;
        unique_kill_process& operator=(unique_kill_process&&) = default;

        unique_kill_process(const unique_kill_process&) = delete;
        unique_kill_process& operator=(const unique_kill_process&) = delete;

        ~unique_kill_process()
        {
            reset();
        }

        HANDLE get() const
        {
            return m_process.get();
        }

        void reset()
        {
            if (m_process)
            {
                if (WaitForSingleObject(m_process.get(), 0) == WAIT_TIMEOUT)
                {
                    LOG_LAST_ERROR_IF(!TerminateProcess(m_process.get(), 0));
                    (void)WaitForSingleObject(m_process.get(), 5000);
                }

                m_process.reset();
            }
        }

        wil::unique_handle m_process;
    };

    static void SignalControllableProcessExit(wil::unique_handle& standardInputWrite)
    {
        if (standardInputWrite)
        {
            DWORD written{};
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(standardInputWrite.get(), "\n", 1, &written, nullptr));
            VERIFY_ARE_EQUAL(1u, written);
            standardInputWrite.reset();
        }
    }

    TEST_METHOD(ConsoleState_WslProcesses_SharedConsole_OutOfOrderRestore)
    {
        wil::unique_hfile conin{CreateFileW(
            L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
        if (!conin)
        {
            LogSkipped("Skipping ConsoleState WSL process test: CONIN$ is not available (no attached console)");
            return;
        }

        DWORD baseline{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(conin.get(), &baseline));
        const DWORD scopeExitRestore = baseline;
        auto restoreBaseline = wil::scope_exit([&] { ::SetConsoleMode(conin.get(), scopeExitRestore); });

        auto [aOutRead, aOutWrite] = CreateSubprocessPipe(false, true);
        auto [aInRead, aInWrite] = CreateSubprocessPipe(true, false);
        unique_kill_process processA(StartControllableWslProcess(aInRead.get(), aOutWrite.get()));
        VERIFY_IS_TRUE(processA.get() != nullptr);
        aOutWrite.reset();
        aInRead.reset();

        PartialHandleRead outputA(aOutRead.get());
        outputA.Expect("ready");

        DWORD configured{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(conin.get(), &configured));
        VERIFY_ARE_NOT_EQUAL(baseline, configured);

        auto [bOutRead, bOutWrite] = CreateSubprocessPipe(false, true);
        auto [bInRead, bInWrite] = CreateSubprocessPipe(true, false);
        unique_kill_process processB(StartControllableWslProcess(bInRead.get(), bOutWrite.get()));
        VERIFY_IS_TRUE(processB.get() != nullptr);
        bOutWrite.reset();
        bInRead.reset();

        PartialHandleRead outputB(bOutRead.get());
        outputB.Expect("ready");

        SignalControllableProcessExit(aInWrite);
        VERIFY_ARE_EQUAL(0u, WaitForProcessExit(processA.get(), 15000));

        VERIFY_ARE_EQUAL(
            static_cast<DWORD>(WAIT_TIMEOUT),
            WaitForSingleObject(processB.get(), 0),
            L"Process B must still be alive when process A exits for out-of-order restore coverage");

        DWORD afterA{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(conin.get(), &afterA));
        VERIFY_IS_TRUE(
            (afterA == baseline) || (afterA == configured),
            L"Out-of-order process teardown may restore baseline early; "
            L"only the final mode after all clients exit is guaranteed");

        SignalControllableProcessExit(bInWrite);
        VERIFY_ARE_EQUAL(0u, WaitForProcessExit(processB.get(), 15000));

        DWORD finalMode{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(conin.get(), &finalMode));
        VERIFY_ARE_EQUAL(baseline, finalMode);
    }

    TEST_METHOD(ConsoleState_WslProcesses_SeparateConsoles_Isolation)
    {
        wil::unique_hfile conin{CreateFileW(
            L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)};
        if (!conin)
        {
            LogSkipped("Skipping ConsoleState separate-console test: CONIN$ is not available (no attached console)");
            return;
        }

        DWORD baseline{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(conin.get(), &baseline));

        auto [outRead, outWrite] = CreateSubprocessPipe(false, true);
        auto [inRead, inWrite] = CreateSubprocessPipe(true, false);
        unique_kill_process process(StartControllableWslProcess(
            inRead.get(), outWrite.get(), CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT));
        VERIFY_IS_TRUE(process.get() != nullptr);
        outWrite.reset();
        inRead.reset();

        PartialHandleRead output(outRead.get());
        output.Expect("ready");

        DWORD duringOtherConsole{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(conin.get(), &duringOtherConsole));
        VERIFY_ARE_EQUAL(
            baseline,
            duringOtherConsole,
            L"A WSL process launched in CREATE_NEW_CONSOLE must not mutate this console's input mode");

        SignalControllableProcessExit(inWrite);
        VERIFY_ARE_EQUAL(0u, WaitForProcessExit(process.get(), 15000));

        DWORD finalMode{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetConsoleMode(conin.get(), &finalMode));
        VERIFY_ARE_EQUAL(baseline, finalMode);
    }

    TEST_METHOD(StringHelpers)
    {
        std::string string1 = "aaaBBB";
        std::string string2 = "aaabbb";
        VERIFY_IS_TRUE(wsl::shared::string::IsEqual(string1, string2, true));
        VERIFY_IS_FALSE(wsl::shared::string::IsEqual(string1, string2, false));
        VERIFY_IS_TRUE(wsl::shared::string::IsEqual(string1.c_str(), string2.c_str(), true));
        VERIFY_IS_FALSE(wsl::shared::string::IsEqual(string1.c_str(), string2.c_str(), false));
        VERIFY_IS_TRUE(wsl::shared::string::StartsWith(string1, string2.substr(0, 3), true));
        VERIFY_IS_FALSE(wsl::shared::string::StartsWith(string1, string2, false));

        std::wstring wstring1 = L"aaaBBB";
        std::wstring wstring2 = L"aaabbb";
        VERIFY_IS_TRUE(wsl::shared::string::IsEqual(wstring1, wstring2, true));
        VERIFY_IS_FALSE(wsl::shared::string::IsEqual(wstring1, wstring2, false));
        VERIFY_IS_TRUE(wsl::shared::string::IsEqual(wstring1.c_str(), wstring2.c_str(), true));
        VERIFY_IS_FALSE(wsl::shared::string::IsEqual(wstring1.c_str(), wstring2.c_str(), false));
        VERIFY_IS_TRUE(wsl::shared::string::StartsWith(wstring1, wstring2.substr(0, 3), true));
        VERIFY_IS_FALSE(wsl::shared::string::StartsWith(wstring1, wstring2, false));

        // Test wsl::shared::string::ParseBool
        std::vector<std::pair<LPCSTR, std::optional<bool>>> boolTests = {
            {"1", true},
            {"0", false},
            {"true", true},
            {"false", false},
            {"True", true},
            {"False", false},
            {"t", std::nullopt},
            {"f", std::nullopt},
            {"T", std::nullopt},
            {"F", std::nullopt},
            {nullptr, std::nullopt},
            {"", std::nullopt},
            {"2", std::nullopt},
            {"true_", std::nullopt},
            {"false_", std::nullopt},
        };

        for (const auto& [input, expected] : boolTests)
        {
            VERIFY_ARE_EQUAL(expected, wsl::shared::string::ParseBool(input));

            std::wstring wideString = wsl::shared::string::MultiByteToWide(input);
            VERIFY_ARE_EQUAL(expected, wsl::shared::string::ParseBool(wideString.c_str()));
        }

        // With AllowExtendedForms the single-letter "t"/"f" forms (case-insensitive) are also
        // recognized, matching Go's strconv.ParseBool (and therefore the Docker CLI). Every
        // form accepted by default must still parse identically in extended mode.
        std::vector<std::pair<LPCSTR, std::optional<bool>>> extendedBoolTests = {
            {"1", true},
            {"0", false},
            {"true", true},
            {"false", false},
            {"True", true},
            {"False", false},
            {"t", true},
            {"T", true},
            {"f", false},
            {"F", false},
            {nullptr, std::nullopt},
            {"", std::nullopt},
            {"2", std::nullopt},
            {"tr", std::nullopt},
            {"true_", std::nullopt},
            {"false_", std::nullopt},
        };

        for (const auto& [input, expected] : extendedBoolTests)
        {
            VERIFY_ARE_EQUAL(expected, wsl::shared::string::ParseBool(input, true));

            std::wstring wideString = wsl::shared::string::MultiByteToWide(input);
            VERIFY_ARE_EQUAL(expected, wsl::shared::string::ParseBool(wideString.c_str(), true));
        }

        // Test wsl::shared::string::ParseMemoryString
        const std::vector<std::pair<LPCSTR, std::optional<uint64_t>>> testCases{
            {"0", 0},
            {"1", 1},
            {" 1", 1},
            {"1B", 1},
            {"1K", 1024},
            {"1KB", 1024},
            {"2M", 2 * 1024 * 1024},
            {"100MB", 100 * 1024 * 1024},
            {"9G", 9 * 1024ULL * 1024ULL * 1024ULL},
            {"44GB", 44 * 1024ULL * 1024ULL * 1024ULL},
            {"1TB", 1ULL << 40},
            {"2T", 2ULL << 40},
            {"1 B", std::nullopt},
            {nullptr, std::nullopt},
            {"", std::nullopt},
            {"foo", std::nullopt}};

        for (const auto& [input, expected] : testCases)
        {
            VERIFY_ARE_EQUAL(wsl::shared::string::ParseMemorySize(input), expected);

            const auto wideInput = wsl::shared::string::MultiByteToWide(input);
            VERIFY_ARE_EQUAL(wsl::shared::string::ParseMemorySize(wideInput.c_str()), expected);
        }

        // Test wsl::shared::string GUID helpers
        const GUID guid = {0x1234567a, 0x1234, 0x5678, {0x12, 0x34, 0x56, 0x78, 0x12, 0x34, 0x56, 0x78}};
        const std::string guidString = "{1234567a-1234-5678-1234-567812345678}";
        const std::string guidStringNoBraces = "1234567a-1234-5678-1234-567812345678";
        const std::vector<std::pair<LPCSTR, std::optional<GUID>>> guidTestCases{
            {guidString.c_str(), guid},
            {guidStringNoBraces.c_str(), guid},
            {nullptr, std::nullopt},
            {"", std::nullopt},
            {"foo", std::nullopt},
            {"1234567G-1234-5678-1234-5678123456789", std::nullopt},
            {"{1234567a-1234-5678-1234-567812345678", std::nullopt},
            {"{1234567aB-1234-5678-1234-567812345678}", std::nullopt}};

        for (const auto& [input, expected] : guidTestCases)
        {
            VERIFY_ARE_EQUAL(expected, wsl::shared::string::ToGuid(input));
            const auto wideInput = wsl::shared::string::MultiByteToWide(input);
            VERIFY_ARE_EQUAL(expected, wsl::shared::string::ToGuid(wideInput));
        }

        VERIFY_ARE_EQUAL(guidString, wsl::shared::string::GuidToString<char>(guid));
        VERIFY_ARE_EQUAL(guidString, wsl::shared::string::GuidToString<char>(guid, wsl::shared::string::GuidToStringFlags::AddBraces));
        VERIFY_ARE_EQUAL(guidStringNoBraces, wsl::shared::string::GuidToString<char>(guid, wsl::shared::string::GuidToStringFlags::None));

        auto upperCaseGuidString = guidStringNoBraces;
        std::transform(upperCaseGuidString.begin(), upperCaseGuidString.end(), upperCaseGuidString.begin(), toupper);
        VERIFY_ARE_EQUAL(upperCaseGuidString, wsl::shared::string::GuidToString<char>(guid, wsl::shared::string::GuidToStringFlags::Uppercase));

        const auto wideGuidString = wsl::shared::string::MultiByteToWide(guidString);
        VERIFY_ARE_EQUAL(wideGuidString, wsl::shared::string::GuidToString<wchar_t>(guid));

        VERIFY_ARE_EQUAL(wideGuidString, wsl::shared::string::GuidToString<wchar_t>(guid, wsl::shared::string::GuidToStringFlags::AddBraces));
        const auto wideGuidStringNoBraces = wsl::shared::string::MultiByteToWide(guidStringNoBraces);
        VERIFY_ARE_EQUAL(wideGuidStringNoBraces, wsl::shared::string::GuidToString<wchar_t>(guid, wsl::shared::string::GuidToStringFlags::None));

        auto upperCaseGuidStringWide = wideGuidStringNoBraces;
        std::transform(upperCaseGuidStringWide.begin(), upperCaseGuidStringWide.end(), upperCaseGuidStringWide.begin(), toupper);
        VERIFY_ARE_EQUAL(upperCaseGuidStringWide, wsl::shared::string::GuidToString<wchar_t>(guid, wsl::shared::string::GuidToStringFlags::Uppercase));
    }

    TEST_METHOD(WindowsPathWithSpaces)
    {
        wil::unique_environstrings_ptr originalPath;
        const DWORD pathLength = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        if (pathLength > 0)
        {
            originalPath.reset(static_cast<PWSTR>(HeapAlloc(GetProcessHeap(), 0, pathLength * sizeof(wchar_t))));
            THROW_LAST_ERROR_IF_NULL(originalPath.get());
            THROW_LAST_ERROR_IF(GetEnvironmentVariableW(L"PATH", originalPath.get(), pathLength) == 0);
        }

        auto cleanup = wil::scope_exit([&]() {
            if (originalPath)
            {
                THROW_LAST_ERROR_IF(!SetEnvironmentVariableW(L"PATH", originalPath.get()));
            }
        });

        const wchar_t* testPath =
            L"C:\\Program Files\\Git\\cmd;"
            L"C:\\Program Files\\PowerShell\\7;"
            L"C:\\Program Files (x86)\\Common Files;"
            L"C:\\Users\\Test User\\AppData\\Local\\Programs\\Microsoft VS Code\\bin";

        THROW_LAST_ERROR_IF(!SetEnvironmentVariableW(L"PATH", testPath));

        auto [output, _] = LxsstuLaunchWslAndCaptureOutput(L"echo $PATH");

        VERIFY_IS_TRUE(output.find(L"/mnt/c/Program Files/Git/cmd") != std::wstring::npos);
        VERIFY_IS_TRUE(output.find(L"/mnt/c/Program Files/PowerShell/7") != std::wstring::npos);
        VERIFY_IS_TRUE(output.find(L"/mnt/c/Program Files (x86)/Common Files") != std::wstring::npos);
        VERIFY_IS_TRUE(output.find(L"/mnt/c/Users/Test User/AppData/Local/Programs/Microsoft VS Code/bin") != std::wstring::npos);
    }
};
} // namespace SimpleTests