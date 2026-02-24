/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    Plan9Tests.cpp

Abstract:

    This file contains test cases for the plan9 logic.

--*/

#include "precomp.h"
#include "Common.h"

#define LXSST_P9_PREFIX L"\\\\wsl.localhost\\" LXSS_DISTRO_NAME_TEST_L
#define LXSST_P9_TEST_DIR LXSST_P9_PREFIX L"\\data\\p9_test"
#define LXSST_P9_CLEANUP_COMMAND_LINE L"/bin/bash -c \"rm -rf /data/p9_test\""

#define VERIFY_LAST_ERROR(error) VERIFY_ARE_EQUAL(static_cast<DWORD>(error), GetLastError())

namespace Plan9Tests {
class Plan9Tests
{
    WSL_TEST_CLASS(Plan9Tests)

    // Initialize the tests
    TEST_CLASS_SETUP(TestClassSetup)
    {
        VERIFY_ARE_EQUAL(LxsstuInitialize(TRUE), TRUE);

        const auto result = std::filesystem::create_directories(LXSST_P9_TEST_DIR);
        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            if (!result)
            {
                auto [out, _] = LxsstuLaunchPowershellAndCaptureOutput(L"(Get-Service P9rdr).Status", 0);
                LogInfo("p9rdr state: %s", out.c_str());
                VERIFY_NO_THROW(LxsstuUninitialize(TRUE));
            }
        });

        VERIFY_IS_TRUE(result);
        return true;
    }

    // Uninitialize the tests.
    TEST_CLASS_CLEANUP(TestClassCleanup)
    {
        LxsstuLaunchWsl(LXSST_P9_CLEANUP_COMMAND_LINE);
        VERIFY_NO_THROW(LxsstuUninitialize(TRUE));

        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        LxssLogKernelOutput();
        return true;
    }

    // Tests creating a file, writing to it, and reading from it.
    TEST_METHOD(TestReadWriteFile)
    {
        constexpr std::string_view data{"test data"};
        const auto file = CreateNewTestFile(L"\\readwritetest", data);

        VERIFY_WIN32_BOOL_SUCCEEDED(SetFilePointerEx(file.get(), {}, nullptr, FILE_BEGIN));
        char buffer[1024];
        DWORD bytes;
        VERIFY_WIN32_BOOL_SUCCEEDED(ReadFile(file.get(), buffer, sizeof(buffer), &bytes, nullptr));
        VERIFY_ARE_EQUAL(data.size(), bytes);
        VERIFY_ARE_EQUAL(data, std::string_view(buffer, bytes));
    }

    // Tests using a large buffer to read/write a file.
    TEST_METHOD(TestReadWriteFileLarge)
    {
        const auto file = CreateTestFile(L"\\readwritelargetest", FILE_GENERIC_READ | FILE_GENERIC_WRITE, FILE_CREATE);
        char buffer[64 * 1024];
        for (size_t i = 0; i < sizeof(buffer); ++i)
        {
            buffer[i] = i % 26 + 'a';
        }

        for (int i = 0; i < 10; ++i)
        {
            DWORD bytesWritten;
            VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(file.get(), buffer, sizeof(buffer), &bytesWritten, nullptr));
            VERIFY_ARE_EQUAL(sizeof(buffer), bytesWritten);
        }

        VERIFY_WIN32_BOOL_SUCCEEDED(SetFilePointerEx(file.get(), {}, nullptr, FILE_BEGIN));
        char buffer2[64 * 1024];
        DWORD bytesRead;
        for (int i = 0; i < 10; ++i)
        {
            VERIFY_WIN32_BOOL_SUCCEEDED(ReadFile(file.get(), buffer2, sizeof(buffer2), &bytesRead, nullptr));
            VERIFY_ARE_EQUAL(sizeof(buffer), bytesRead);
            VERIFY_IS_TRUE(memcmp(buffer, buffer2, sizeof(buffer)) == 0);
        }

        VERIFY_WIN32_BOOL_SUCCEEDED(ReadFile(file.get(), buffer2, sizeof(buffer2), &bytesRead, nullptr));
        VERIFY_ARE_EQUAL(0u, bytesRead);
    }

    // Tests querying and setting file information.
    TEST_METHOD(TestQuerySetInfo)
    {
        // Check the attributes on the test directory.
        FILE_BASIC_INFO basicInfo{};
        auto file = CreateTestFile({}, FILE_READ_ATTRIBUTES);
        VERIFY_WIN32_BOOL_SUCCEEDED(GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basicInfo, sizeof(basicInfo)));
        VERIFY_IS_TRUE(WI_IsFlagSet(basicInfo.FileAttributes, FILE_ATTRIBUTE_DIRECTORY));
        VERIFY_ARE_NOT_EQUAL(0, basicInfo.ChangeTime.QuadPart);
        VERIFY_ARE_NOT_EQUAL(0, basicInfo.CreationTime.QuadPart);
        VERIFY_ARE_NOT_EQUAL(0, basicInfo.LastAccessTime.QuadPart);
        VERIFY_ARE_NOT_EQUAL(0, basicInfo.LastWriteTime.QuadPart);
        FILE_STANDARD_INFO standardInfo{};
        VERIFY_WIN32_BOOL_SUCCEEDED(GetFileInformationByHandleEx(file.get(), FileStandardInfo, &standardInfo, sizeof(basicInfo)));
        VERIFY_IS_TRUE(standardInfo.Directory);
        VERIFY_IS_FALSE(standardInfo.DeletePending);
        const auto id = GetFileId({});
        VERIFY_ARE_NOT_EQUAL(0ull, id);

        // Check attributes on a file.
        file = CreateNewTestFile(L"\\queryinfotest", "0123456789");
        VERIFY_WIN32_BOOL_SUCCEEDED(GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basicInfo, sizeof(basicInfo)));
        VERIFY_IS_FALSE(WI_IsFlagSet(basicInfo.FileAttributes, FILE_ATTRIBUTE_DIRECTORY));
        VERIFY_ARE_NOT_EQUAL(0, basicInfo.ChangeTime.QuadPart);
        VERIFY_ARE_NOT_EQUAL(0, basicInfo.CreationTime.QuadPart);
        VERIFY_ARE_NOT_EQUAL(0, basicInfo.LastAccessTime.QuadPart);
        VERIFY_ARE_NOT_EQUAL(0, basicInfo.LastWriteTime.QuadPart);
        VERIFY_WIN32_BOOL_SUCCEEDED(GetFileInformationByHandleEx(file.get(), FileStandardInfo, &standardInfo, sizeof(basicInfo)));
        VERIFY_IS_FALSE(standardInfo.Directory);
        VERIFY_IS_FALSE(standardInfo.DeletePending);
        VERIFY_ARE_EQUAL(1u, standardInfo.NumberOfLinks);
        VERIFY_ARE_EQUAL(10, standardInfo.EndOfFile.QuadPart);
        const auto id2 = GetFileId(L"\\queryinfotest");
        VERIFY_ARE_NOT_EQUAL(0ull, id2);
        VERIFY_ARE_NOT_EQUAL(id, id2);

        // Try truncating the file.
        LARGE_INTEGER size;
        size.QuadPart = 5;
        VERIFY_WIN32_BOOL_SUCCEEDED(SetFilePointerEx(file.get(), size, nullptr, FILE_BEGIN));
        VERIFY_WIN32_BOOL_SUCCEEDED(SetEndOfFile(file.get()));
        VERIFY_WIN32_BOOL_SUCCEEDED(GetFileInformationByHandleEx(file.get(), FileStandardInfo, &standardInfo, sizeof(basicInfo)));
        VERIFY_ARE_EQUAL(5, standardInfo.EndOfFile.QuadPart);
    }

    // Tests deleting files and directories.
    TEST_METHOD(TestDelete)
    {
        // Delete a file.
        CreateNewTestFile(L"\\deletetestfile", "0123456789");
        VERIFY_IS_TRUE(CheckFileExists(L"\\deletetestfile"));
        VERIFY_WIN32_BOOL_SUCCEEDED(DeleteFileW(LXSST_P9_TEST_DIR L"\\deletetestfile"));
        VERIFY_IS_FALSE(CheckFileExists(L"\\deletetestfile"));

        // Delete a directory.
        VERIFY_WIN32_BOOL_SUCCEEDED(CreateDirectory(LXSST_P9_TEST_DIR L"\\deletetestdir", nullptr));
        VERIFY_IS_TRUE(CheckFileExists(L"\\deletetestdir"));
        VERIFY_WIN32_BOOL_SUCCEEDED(RemoveDirectory(LXSST_P9_TEST_DIR L"\\deletetestdir"));
        VERIFY_IS_FALSE(CheckFileExists(L"\\deletetestdir"));

        // Try to delete non-empty directory.
        VERIFY_WIN32_BOOL_SUCCEEDED(CreateDirectory(LXSST_P9_TEST_DIR L"\\deletetestdir", nullptr));
        CreateNewTestFile(L"\\deletetestdir\\testfile", "0123456789");
        VERIFY_WIN32_BOOL_FAILED(RemoveDirectory(LXSST_P9_TEST_DIR L"\\deletetestdir"));
        VERIFY_LAST_ERROR(ERROR_DIR_NOT_EMPTY);
        VERIFY_IS_TRUE(CheckFileExists(L"\\deletetestdir"));
    }

    // Tests renaming files and directories.
    TEST_METHOD(TestRename)
    {
        // Rename a file.
        CreateNewTestFile(L"\\renametestfile", "0123456789");
        auto id = GetFileId(L"\\renametestfile");
        VERIFY_WIN32_BOOL_SUCCEEDED(MoveFile(LXSST_P9_TEST_DIR L"\\renametestfile", LXSST_P9_TEST_DIR L"\\renametestfile2"));
        auto id2 = GetFileId(L"\\renametestfile2");
        VERIFY_ARE_EQUAL(id, id2);
        VERIFY_IS_FALSE(CheckFileExists(L"\\renametestfile"));
        CreateNewTestFile(L"\\renametestfile", "abcdefg");
        id = GetFileId(L"\\renametestfile");
        VERIFY_ARE_NOT_EQUAL(id, id2);
        VERIFY_WIN32_BOOL_FAILED(MoveFile(LXSST_P9_TEST_DIR L"\\renametestfile", LXSST_P9_TEST_DIR L"\\renametestfile2"));
        VERIFY_LAST_ERROR(ERROR_ALREADY_EXISTS);
        VERIFY_WIN32_BOOL_SUCCEEDED(
            MoveFileEx(LXSST_P9_TEST_DIR L"\\renametestfile", LXSST_P9_TEST_DIR L"\\renametestfile2", MOVEFILE_REPLACE_EXISTING));

        id2 = GetFileId(L"\\renametestfile2");
        VERIFY_ARE_EQUAL(id, id2);

        // Rename a directory
        VERIFY_WIN32_BOOL_SUCCEEDED(CreateDirectory(LXSST_P9_TEST_DIR L"\\renametestdir", nullptr));
        id = GetFileId(L"\\renametestdir");
        VERIFY_WIN32_BOOL_SUCCEEDED(MoveFile(LXSST_P9_TEST_DIR L"\\renametestdir", LXSST_P9_TEST_DIR L"\\renametestdir2"));
        id2 = GetFileId(L"\\renametestdir2");
        VERIFY_ARE_EQUAL(id, id2);
        VERIFY_IS_FALSE(CheckFileExists(L"\\renametestdir"));

        // Directory over a file.
        VERIFY_WIN32_BOOL_FAILED(
            MoveFileEx(LXSST_P9_TEST_DIR L"\\renametestdir2", LXSST_P9_TEST_DIR L"\\renametestfile2", MOVEFILE_REPLACE_EXISTING));

        VERIFY_LAST_ERROR(ERROR_DIRECTORY);

        // File over a directory.
        VERIFY_WIN32_BOOL_FAILED(
            MoveFileEx(LXSST_P9_TEST_DIR L"\\renametestfile2", LXSST_P9_TEST_DIR L"\\renametestdir2", MOVEFILE_REPLACE_EXISTING));

        VERIFY_LAST_ERROR(ERROR_ACCESS_DENIED);
    }

    // Tests listing the files in a directory.
    TEST_METHOD(TestReadDir)
    {
        constexpr int fileCount = 500;
        VERIFY_WIN32_BOOL_SUCCEEDED(CreateDirectory(LXSST_P9_TEST_DIR L"\\readdirtest", nullptr));
        for (int i = 0; i < fileCount; ++i)
        {
            wchar_t path[MAX_PATH]{};
            swprintf_s(path, L"\\readdirtest\\%d", i);
            CreateNewTestFile(path, "0123456789");
        }

        WIN32_FIND_DATA findData{};
        const wil::unique_hfind find{FindFirstFile(LXSST_P9_TEST_DIR L"\\readdirtest\\*", &findData)};
        VERIFY_WIN32_BOOL_SUCCEEDED(static_cast<bool>(find));
        int count{};
        bool foundFiles[fileCount]{};
        do
        {
            ++count;
            VERIFY_ARE_NOT_EQUAL(0u, findData.dwFileAttributes);
            VERIFY_ARE_NOT_EQUAL(0ull, *reinterpret_cast<PULONGLONG>(&findData.ftCreationTime));
            VERIFY_ARE_NOT_EQUAL(0ull, *reinterpret_cast<PULONGLONG>(&findData.ftLastAccessTime));
            VERIFY_ARE_NOT_EQUAL(0ull, *reinterpret_cast<PULONGLONG>(&findData.ftLastWriteTime));
            if (findData.cFileName[0] != L'.')
            {
                VERIFY_ARE_EQUAL(0u, findData.nFileSizeHigh);
                VERIFY_ARE_EQUAL(10u, findData.nFileSizeLow);
                int file = wcstol(findData.cFileName, nullptr, 10);
                VERIFY_IS_GREATER_THAN_OR_EQUAL(file, 0);
                VERIFY_IS_LESS_THAN(file, fileCount);
                VERIFY_IS_FALSE(foundFiles[file]);
                foundFiles[file] = true;
            }

        } while (FindNextFile(find.get(), &findData));

        VERIFY_LAST_ERROR(ERROR_NO_MORE_FILES);
        VERIFY_ARE_EQUAL(fileCount + 2, count);

        for (int i = 0; i < fileCount; ++i)
        {
            VERIFY_IS_TRUE(foundFiles[i]);
        }
    }

    // Tests using mount points inside the WSL instance.
    TEST_METHOD(TestMounts)
    {
        // Check access into mounts like procfs is allowed.
        wil::unique_hfile file{CreateFile(
            LXSST_P9_PREFIX L"\\proc\\stat",
            FILE_GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr)};

        VERIFY_WIN32_BOOL_SUCCEEDED(static_cast<bool>(file));

        char buffer[1024];
        DWORD bytes;
        VERIFY_WIN32_BOOL_SUCCEEDED(ReadFile(file.get(), buffer, sizeof(buffer), &bytes, nullptr));
        VERIFY_IS_GREATER_THAN(bytes, 0u);

        // Check access into drvfs mounts is not allowed.
        file.reset(CreateFile(
            LXSST_P9_PREFIX L"\\mnt\\c",
            FILE_GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr));

        VERIFY_IS_FALSE(static_cast<bool>(file));
        VERIFY_LAST_ERROR(ERROR_ACCESS_DENIED);

        file.reset(CreateFile(
            LXSST_P9_PREFIX L"\\mnt\\c\\Windows",
            FILE_GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr));

        VERIFY_IS_FALSE(static_cast<bool>(file));
        VERIFY_LAST_ERROR(ERROR_ACCESS_DENIED);
    }

    TEST_METHOD(TestCreate)
    {
        wil::unique_hfile file;
        IO_STATUS_BLOCK ioStatus;

        // Check error codes for non-existing files.
        auto status = CreateFileNt(&file, LXSST_P9_PREFIX L"\\dat\\p9_test", FILE_GENERIC_READ, ioStatus);
        VERIFY_ARE_EQUAL(STATUS_OBJECT_PATH_NOT_FOUND, status);
        status = CreateFileNt(&file, LXSST_P9_PREFIX L"\\data\\foo", FILE_GENERIC_READ, ioStatus);
        VERIFY_ARE_EQUAL(STATUS_OBJECT_NAME_NOT_FOUND, status);
        status = CreateFileNt(&file, LXSST_P9_PREFIX L"\\etc\\resolve.conf\\foo", FILE_GENERIC_READ, ioStatus);
        VERIFY_ARE_EQUAL(STATUS_OBJECT_PATH_NOT_FOUND, status);

        // Create a file.
        VERIFY_NT_SUCCESS(CreateFileNt(&file, LXSST_P9_TEST_DIR L"\\testfile", FILE_GENERIC_WRITE, ioStatus, FILE_CREATE));
        VERIFY_ARE_EQUAL(static_cast<ULONG_PTR>(FILE_CREATED), ioStatus.Information);

        // Write some test content.
        const std::string contents{"hello"};
        DWORD bytes;
        VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(file.get(), contents.data(), static_cast<DWORD>(contents.size()), &bytes, nullptr));
        VERIFY_ARE_EQUAL(contents.size(), bytes);
        file.reset();

        // Exclusive create should fail now.
        status = CreateFileNt(&file, LXSST_P9_TEST_DIR L"\\testfile", FILE_GENERIC_READ, ioStatus, FILE_CREATE);
        VERIFY_ARE_EQUAL(STATUS_OBJECT_NAME_COLLISION, status);

        // Open-if existing file.
        VERIFY_NT_SUCCESS(CreateFileNt(&file, LXSST_P9_TEST_DIR L"\\testfile", FILE_GENERIC_READ, ioStatus, FILE_OPEN_IF));
        VERIFY_ARE_EQUAL(static_cast<ULONG_PTR>(FILE_OPENED), ioStatus.Information);
        LARGE_INTEGER size;
        VERIFY_WIN32_BOOL_SUCCEEDED(GetFileSizeEx(file.get(), &size));
        VERIFY_ARE_EQUAL(5, size.QuadPart);

        // Open-if new file.
        VERIFY_NT_SUCCESS(CreateFileNt(&file, LXSST_P9_TEST_DIR L"\\testfile2", FILE_GENERIC_READ, ioStatus, FILE_OPEN_IF));
        VERIFY_ARE_EQUAL(static_cast<ULONG_PTR>(FILE_CREATED), ioStatus.Information);

        // Overwrite non-existing file.
        status = CreateFileNt(&file, LXSST_P9_TEST_DIR L"\\testfile3", FILE_GENERIC_WRITE, ioStatus, FILE_OVERWRITE);
        VERIFY_ARE_EQUAL(STATUS_OBJECT_NAME_NOT_FOUND, status);

        VERIFY_NT_SUCCESS(CreateFileNt(&file, LXSST_P9_TEST_DIR L"\\testfile3", FILE_GENERIC_WRITE, ioStatus, FILE_OVERWRITE_IF));
        VERIFY_ARE_EQUAL(static_cast<ULONG_PTR>(FILE_CREATED), ioStatus.Information);

        // Overwrite existing file.
        VERIFY_NT_SUCCESS(CreateFileNt(&file, LXSST_P9_TEST_DIR L"\\testfile", FILE_GENERIC_WRITE, ioStatus, FILE_OVERWRITE));
        VERIFY_ARE_EQUAL(static_cast<ULONG_PTR>(FILE_OVERWRITTEN), ioStatus.Information);
        VERIFY_WIN32_BOOL_SUCCEEDED(GetFileSizeEx(file.get(), &size));
        VERIFY_ARE_EQUAL(0, size.QuadPart);
        VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(file.get(), contents.data(), static_cast<DWORD>(contents.size()), &bytes, nullptr));
        VERIFY_ARE_EQUAL(contents.size(), bytes);
        file.reset();
        VERIFY_NT_SUCCESS(CreateFileNt(&file, LXSST_P9_TEST_DIR L"\\testfile", FILE_GENERIC_WRITE, ioStatus, FILE_OVERWRITE_IF));
        VERIFY_ARE_EQUAL(static_cast<ULONG_PTR>(FILE_OVERWRITTEN), ioStatus.Information);
        VERIFY_WIN32_BOOL_SUCCEEDED(GetFileSizeEx(file.get(), &size));
        VERIFY_ARE_EQUAL(0, size.QuadPart);

        // Open a directory with FILE_NON_DIRECTORY_FILE.
        status = CreateFileNt(&file, LXSST_P9_TEST_DIR, FILE_GENERIC_READ, ioStatus, FILE_OPEN, 0, FILE_NON_DIRECTORY_FILE);
        VERIFY_ARE_EQUAL(STATUS_FILE_IS_A_DIRECTORY, status);

        // Open a file with FILE_DIRECTORY_FILE.
        status = CreateFileNt(&file, LXSST_P9_TEST_DIR L"\\testfile", FILE_GENERIC_READ, ioStatus, FILE_OPEN, 0, FILE_DIRECTORY_FILE);
        VERIFY_ARE_EQUAL(STATUS_NOT_A_DIRECTORY, status);
    }

    static auto EnablePlan9Logging()
    {
        LxssWriteWslDistroConfig("[fileServer]\nlogFile=/plan9-logs.txt\nlogTruncate=false\nlogLevel=5");

        return wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [] {
            // clean up wsl.conf file
            LxsstuLaunchWsl(L"rm /etc/wsl.conf");
            TerminateDistribution();
        });
    }

    TEST_METHOD(TestPlan9ServerTimeout)
    {
        // This test has proven to be unstable, most likely because another program opens a file inside the distro, which prevents it from terminating.
        SKIP_TEST_UNSTABLE();

        auto revertLogging = EnablePlan9Logging();

        auto dumpLogs = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, []() {
            const auto output = LxsstuLaunchWslAndCaptureOutput(L"cat /plan9-logs.txt");
            LogInfo("Plan9 logs: %s", output.first.c_str());
        });

        wsl::windows::common::SvcComm service;
        auto distro = service.GetDefaultDistribution();
        service.TerminateInstance(&distro);

        auto getDistroState = [distro, &service]() -> LxssDistributionState {
            auto distros = service.EnumerateDistributions();
            const auto it =
                std::find_if(distros.begin(), distros.end(), [&](const auto& e) { return IsEqualGUID(distro, e.DistroGuid); });

            VERIFY_ARE_NOT_EQUAL(it, distros.end());

            return it->State;
        };

        VERIFY_ARE_EQUAL(getDistroState(), LxssDistributionStateInstalled);

        // Open a file via \\wsl.localhost and validate that the distro does not terminate
        auto file = CreateTestFile(L"\\9p-test-file", GENERIC_ALL, FILE_CREATE, FILE_FLAG_DELETE_ON_CLOSE);

        // Now the distro should be running
        VERIFY_ARE_EQUAL(getDistroState(), LxssDistributionStateRunning);

        // Validate that the distro does not terminate until the file is closed
        // Note: Distributions time out after 10 seconds.
        std::this_thread::sleep_for(std::chrono::seconds(20));

        // Close the file and make sure that the distro terminates
        file.reset();

        // The distro should now time out and stop
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
        while (std::chrono::steady_clock::now() < deadline && getDistroState() != LxssDistributionStateInstalled)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        VERIFY_ARE_EQUAL(getDistroState(), LxssDistributionStateInstalled);
    }

    TEST_METHOD(TestPlan9AdditionalGroupAccess)
    {
        ULONG Uid{};
        ULONG Gid{};

        // Create a user for this test
        CreateUser(L"plan9testuser", &Uid, &Gid);

        // Create a folder that's unaccessible to plan9testuser
        VERIFY_ARE_EQUAL(
            LxsstuLaunchWsl(
                L"mkdir -p /tmp/plan9-group-test && groupadd -f plan9testgroup && chown root:plan9testgroup "
                L"/tmp/plan9-group-test && "
                L"echo -n foo > /tmp/plan9-group-test/bar && chmod 770 /tmp/plan9-group-test"),
            0u);

        auto cleanup = wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() {
            LxsstuLaunchWsl(L"-u root rm -rf /etc/wsl.conf /tmp/plan9-group-test");
            TerminateDistribution();
        });

        // Make plan9testuser the default
        LxssWriteWslDistroConfig("[user]\ndefault=plan9testuser\n");
        TerminateDistribution();

        // Validate that folder isn't accessible
        constexpr auto path = L"\\\\wsl.localhost\\" LXSS_DISTRO_NAME_TEST "\\tmp\\plan9-group-test\\bar";
        std::wifstream file(path);
        VERIFY_IS_FALSE(file.good());

        // Add plan9testuser to plan9testgroup
        VERIFY_ARE_EQUAL(LxsstuLaunchWsl(L"-u root usermod -G plan9testgroup -a plan9testuser"), 0u);

        // Validate that the file can be accessed now
        TerminateDistribution();
        // There's a race condition on fe_release that can cause opening this file to fail.
        try
        {
            wsl::shared::retry::RetryWithTimeout<void>(
                [&file]() {
                    file.open(path);
                    LogInfo("Failed to open %ls, %d", path, errno);
                    THROW_HR_IF(E_ABORT, !file.good());
                },
                std::chrono::seconds(1),
                std::chrono::minutes(2));
        }
        catch (...)
        {
            LogError("Timed out trying to open: %ls", path);
            VERIFY_FAIL();
        }

        std::wstring content(3, '\0');
        VERIFY_IS_TRUE(file.read(content.data(), content.size()).good());

        VERIFY_ARE_EQUAL(content, L"foo");
    }

    /* Plan9 Test Helper Methods */

    static wil::unique_hfile CreateTestFile(std::wstring_view path, DWORD desiredAccess, DWORD disposition = OPEN_EXISTING, DWORD flags = 0)
    {
        std::wstring fullPath{LXSST_P9_TEST_DIR};
        fullPath += path;
        wil::unique_hfile file{CreateFile(
            fullPath.c_str(),
            desiredAccess,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            disposition,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS | flags,
            nullptr)};

        VERIFY_WIN32_BOOL_SUCCEEDED(static_cast<bool>(file));

        return file;
    }

    wil::unique_hfile CreateNewTestFile(const std::wstring& path, std::string_view contents)
    {
        auto file = CreateTestFile(path, FILE_GENERIC_WRITE | FILE_GENERIC_READ, CREATE_NEW);
        DWORD bytes;
        VERIFY_WIN32_BOOL_SUCCEEDED(WriteFile(file.get(), contents.data(), static_cast<DWORD>(contents.size()), &bytes, nullptr));
        VERIFY_ARE_EQUAL(contents.size(), bytes);
        return file;
    }

    static NTSTATUS CreateFileNt(
        PHANDLE handle, LPCWSTR name, ACCESS_MASK desiredAccess, IO_STATUS_BLOCK& ioStatus, ULONG disposition = FILE_OPEN, ULONG attributes = 0, ULONG createOptions = 0)
    {
        UNICODE_STRING pathu;
        THROW_IF_NTSTATUS_FAILED(RtlDosPathNameToNtPathName_U_WithStatus(name, &pathu, nullptr, nullptr));
        wil::unique_process_heap_ptr<WCHAR> buffer{pathu.Buffer};
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(&oa, &pathu, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
        return NtCreateFile(
            handle,
            desiredAccess | SYNCHRONIZE,
            &oa,
            &ioStatus,
            nullptr,
            attributes,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            disposition,
            createOptions | FILE_SYNCHRONOUS_IO_ALERT,
            nullptr,
            0);
    }

    static bool CheckFileExists(std::wstring_view path)
    {
        std::wstring fullPath{LXSST_P9_TEST_DIR};
        fullPath += path;
        if (!PathFileExists(fullPath.c_str()))
        {
            VERIFY_LAST_ERROR(ERROR_FILE_NOT_FOUND);
            return false;
        }

        return true;
    }

    ULONGLONG GetFileId(std::wstring_view path)
    {
        BY_HANDLE_FILE_INFORMATION info;
        const auto file = CreateTestFile(path, FILE_READ_ATTRIBUTES);
        VERIFY_WIN32_BOOL_SUCCEEDED(GetFileInformationByHandle(file.get(), &info));
        return static_cast<ULONGLONG>(info.nFileIndexHigh) << 32 | info.nFileIndexLow;
    }
};
} // namespace Plan9Tests