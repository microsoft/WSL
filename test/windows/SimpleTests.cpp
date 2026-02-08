/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    SimpleTests.cpp

Abstract:

    This file contains smoke tests for WSL.

--*/

#include "precomp.h"
#include "Common.h"

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

    TEST_METHOD(CheckSparse)
    {
        WSL2_TEST_ONLY();

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

    TEST_METHOD(CreateCpioInitrd)
    {
        using wsl::windows::common::filesystem::CreateCpioInitrd;
        using wsl::windows::common::filesystem::TempFile;
        using wsl::windows::common::filesystem::TempFileFlags;

        auto validateCpio = [](size_t sourceSize) {
            // Create source file with specified size
            TempFile sourceFile(GENERIC_WRITE, 0, CREATE_ALWAYS, TempFileFlags::None);
            std::vector<char> sourceData(sourceSize, 'X');
            DWORD written;
            THROW_IF_WIN32_BOOL_FALSE(
                WriteFile(sourceFile.Handle.get(), sourceData.data(), static_cast<DWORD>(sourceData.size()), &written, nullptr));
            sourceFile.Handle.reset();

            // Create CPIO archive
            TempFile destFile(0, 0, CREATE_ALWAYS, TempFileFlags::None, L"img");
            CreateCpioInitrd(sourceFile.Path, destFile.Path);

            // Read and validate the CPIO archive
            wil::unique_hfile cpioHandle{CreateFileW(
                destFile.Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
            THROW_LAST_ERROR_IF(!cpioHandle);

            LARGE_INTEGER cpioSize{};
            THROW_IF_WIN32_BOOL_FALSE(GetFileSizeEx(cpioHandle.get(), &cpioSize));
            VERIFY_ARE_EQUAL(cpioSize.QuadPart % 512, 0LL); // Archive padded to 512-byte boundary

            char header[110] = {};
            DWORD bytesRead;
            THROW_IF_WIN32_BOOL_FALSE(ReadFile(cpioHandle.get(), header, sizeof(header), &bytesRead, nullptr));
            VERIFY_ARE_EQUAL(bytesRead, static_cast<DWORD>(sizeof(header)));

            // Parse CPIO newc header: magic(6) ino mode uid gid nlink mtime filesize devmajor devminor rdevmajor rdevminor namesize check
            DWORD fileSize, nameSize;
            VERIFY_ARE_EQUAL(sscanf_s(header, "070701%*8x%*8x%*8x%*8x%*8x%*8x%8x%*8x%*8x%*8x%*8x%8x", &fileSize, &nameSize), 2);

            // Verify filename matches source file
            auto expectedName = sourceFile.Path.filename().string();
            VERIFY_ARE_EQUAL(nameSize, static_cast<DWORD>(expectedName.size() + 1));

            std::string filename(nameSize, '\0');
            THROW_IF_WIN32_BOOL_FALSE(ReadFile(cpioHandle.get(), filename.data(), nameSize, &bytesRead, nullptr));
            VERIFY_ARE_EQUAL(filename.c_str(), expectedName);

            VERIFY_ARE_EQUAL(fileSize, static_cast<DWORD>(sourceSize));
        };

        // Test various sizes to exercise 4-byte alignment padding
        for (size_t size : {0, 1, 2, 3, 4, 5, 100, 1024, 4096, 65536})
        {
            validateCpio(size);
        }
    }
};
} // namespace SimpleTests