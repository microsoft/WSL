// Copyright (C) Microsoft Corporation. All rights reserved.

#include "precomp.h"
#include "windows/Common.h"
#include "WSLCExecutor.h"
#include "WSLCE2EHelpers.h"

namespace WSLCE2ETests {
using namespace wsl::shared;

class WSLCE2EContainerCpTests
{
    WSLC_TEST_CLASS(WSLCE2EContainerCpTests)

    TEST_CLASS_SETUP(ClassSetup)
    {
        EnsureImageIsLoaded(DebianImage);
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        EnsureImageIsDeleted(DebianImage);
        return true;
    }

    TEST_METHOD_SETUP(MethodSetup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        TarPath = wsl::windows::common::filesystem::GetTempFilename();
        DeleteFileW(TarPath.c_str());
        return true;
    }

    TEST_METHOD_CLEANUP(MethodCleanup)
    {
        EnsureContainerDoesNotExist(WslcContainerName);
        DeleteFileW(TarPath.c_str());
        return true;
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_HelpCommand)
    {
        auto result = RunWslc(L"container cp --help");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stdout.has_value());
        VERIFY_IS_TRUE(result.Stdout->find(L"container cp") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(L"source") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stdout->find(L"target") != std::wstring::npos);
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_EQUAL(L"", result.Stderr.value());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_MissingBothArgs)
    {
        const auto result = RunWslc(L"container cp");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"Required argument not provided: 'source'") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_MissingTarget)
    {
        const auto result = RunWslc(L"container cp -");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_IS_TRUE(result.Stderr->find(L"Required argument not provided: 'target'") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_StdinIsTerminal)
    {
        // Running without piped stdin should fail with a terminal error.
        // RunWslcAndRedirectToFile gives the child a real console stdout handle,
        // and since RunWslc pipes NUL to stdin, we use RunWslcAndRedirectToFile
        // with no output path to get a real console for the child.
        const auto result = RunWslcAndRedirectToFile(L"container cp - fakecontainer:/path");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, result.Stderr.value().size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_SourceNotStdin)
    {
        // Source must be '-' — anything else should fail.
        // Use RunWslc which pipes NUL to stdin (not a terminal).
        const auto result = RunWslc(L"container cp somefile.tar fakecontainer:/path");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, result.Stderr.value().size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_InvalidTargetFormat_NoColon)
    {
        // Target must be CONTAINER:PATH — missing colon should fail.
        const auto result = RunWslc(L"container cp - fakecontainer_nopath");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, result.Stderr.value().size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_InvalidTargetFormat_EmptyContainer)
    {
        // Target with empty container name (:path) should fail.
        const auto result = RunWslc(L"container cp - :/path");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, result.Stderr.value().size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_InvalidTargetFormat_EmptyPath)
    {
        // Target with empty path (container:) should fail.
        const auto result = RunWslc(L"container cp - fakecontainer:");
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, result.Stderr.value().size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ContainerNotFound)
    {
        // Create a valid tar file to pipe in, but target a nonexistent container.
        CreateTestTarFile();

        const auto result = RunWslcWithStdinFile(std::format(L"container cp - {}:/tmp", InvalidContainerName), TarPath);
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, result.Stderr.value().size());
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_Success)
    {
        // Create and start a container with sleep infinity to keep it running.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Create a test tar file with a known file inside.
        CreateTestTarFile();

        // Cp the tar into the running container.
        const auto cpResult = RunWslcWithStdinFile(std::format(L"container cp - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Verify the file was copied by running a command inside the container.
        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_IS_TRUE(execResult.Stdout->find(L"wslc-cp-test-content") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ToStoppedContainer)
    {
        // Create a stopped container (not started).
        auto createResult = RunWslc(std::format(L"container create --name {} {}", WslcContainerName, DebianImage.NameAndTag()));
        createResult.Verify({.Stderr = L"", .ExitCode = 0});

        // Create a test tar file.
        CreateTestTarFile();

        // Attempt to cp into the stopped container — Docker should accept this.
        const auto cpResult = RunWslcWithStdinFile(std::format(L"container cp - {}:/tmp", WslcContainerName), TarPath);

        // Docker's PUT /archive works on stopped containers too.
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveFlag)
    {
        // Create and start a container.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        CreateTestTarFile();

        // Cp with -a flag (archive mode preserves uid/gid).
        const auto cpResult = RunWslcWithStdinFile(std::format(L"container cp -a - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Verify the file was copied.
        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_IS_TRUE(execResult.Stdout->find(L"wslc-cp-test-content") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveFlagLongForm)
    {
        // Create and start a container.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        CreateTestTarFile();

        // Cp with --archive flag (long form).
        const auto cpResult = RunWslcWithStdinFile(std::format(L"container cp --archive - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        // Verify the file was copied.
        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_IS_TRUE(execResult.Stdout->find(L"wslc-cp-test-content") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveFlagEqualsTrue)
    {
        // Test -a=true syntax.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        CreateTestTarFile();

        const auto cpResult =
            RunWslcWithStdinFile(std::format(L"container cp -a=true - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_IS_TRUE(execResult.Stdout->find(L"wslc-cp-test-content") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveFlagEqualsFalse)
    {
        // Test -a=false syntax (no archive mode).
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        CreateTestTarFile();

        const auto cpResult =
            RunWslcWithStdinFile(std::format(L"container cp -a=false - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_IS_TRUE(execResult.Stdout->find(L"wslc-cp-test-content") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveLongFormEqualsTrue)
    {
        // Test --archive=true syntax.
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        CreateTestTarFile();

        const auto cpResult =
            RunWslcWithStdinFile(std::format(L"container cp --archive=true - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_IS_TRUE(execResult.Stdout->find(L"wslc-cp-test-content") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveLongFormEqualsFalse)
    {
        // Test --archive=false syntax (no archive mode).
        auto runResult =
            RunWslc(std::format(L"container run -d --name {} {} sleep infinity", WslcContainerName, DebianImage.NameAndTag()));
        runResult.Verify({.Stderr = L"", .ExitCode = 0});

        CreateTestTarFile();

        const auto cpResult =
            RunWslcWithStdinFile(std::format(L"container cp --archive=false - {}:/tmp", WslcContainerName), TarPath);
        cpResult.Verify({.Stdout = L"", .Stderr = L"", .ExitCode = 0});

        const auto execResult = RunWslc(std::format(L"container exec {} cat /tmp/testfile.txt", WslcContainerName));
        VERIFY_IS_TRUE(execResult.ExitCode.has_value());
        VERIFY_ARE_EQUAL(0u, execResult.ExitCode.value());
        VERIFY_IS_TRUE(execResult.Stdout.has_value());
        VERIFY_IS_TRUE(execResult.Stdout->find(L"wslc-cp-test-content") != std::wstring::npos);
    }

    WSLC_TEST_METHOD(WSLCE2E_Container_Cp_ArchiveFlagInvalidValue)
    {
        // Test -a=invalid should fail with an error.
        CreateTestTarFile();

        const auto result =
            RunWslcWithStdinFile(std::format(L"container cp -a=invalid - {}:/tmp", WslcContainerName), TarPath);
        VERIFY_IS_TRUE(result.ExitCode.has_value());
        VERIFY_ARE_EQUAL(1u, result.ExitCode.value());
        VERIFY_IS_TRUE(result.Stderr.has_value());
        VERIFY_ARE_NOT_EQUAL(0u, result.Stderr.value().size());
    }

private:
    const std::wstring WslcContainerName = L"wslc-test-container-cp";
    const std::wstring InvalidContainerName = L"wslc-nonexistent-container-for-cp";
    const TestImage& DebianImage = DebianTestImage();

    std::filesystem::path TarPath{};

    // Creates a minimal tar file containing a single text file.
    // tar format: 512-byte header + file data padded to 512 bytes + 1024 bytes end-of-archive marker.
    void CreateTestTarFile()
    {
        const std::string fileName = "testfile.txt";
        const std::string fileContent = "wslc-cp-test-content\n";

        // Build a POSIX tar header (512 bytes).
        std::array<char, 512> header{};

        // name (0-99)
        std::copy(fileName.begin(), fileName.end(), header.begin());

        // mode (100-107): 0644
        std::string mode = "0000644";
        std::copy(mode.begin(), mode.end(), header.begin() + 100);

        // uid (108-115): 0
        std::string uid = "0000000";
        std::copy(uid.begin(), uid.end(), header.begin() + 108);

        // gid (116-123): 0
        std::string gid = "0000000";
        std::copy(gid.begin(), gid.end(), header.begin() + 116);

        // size (124-135): octal size
        auto sizeStr = std::format("{:011o}", fileContent.size());
        std::copy(sizeStr.begin(), sizeStr.end(), header.begin() + 124);

        // mtime (136-147): 0
        std::string mtime = "00000000000";
        std::copy(mtime.begin(), mtime.end(), header.begin() + 136);

        // Initialize checksum field with spaces for checksum calculation.
        std::fill(header.begin() + 148, header.begin() + 156, ' ');

        // typeflag (156): '0' (regular file)
        header[156] = '0';

        // Compute checksum: sum of all unsigned bytes in the header.
        unsigned int checksum = 0;
        for (unsigned char c : header)
        {
            checksum += c;
        }
        auto checksumStr = std::format("{:06o}", checksum);
        std::copy(checksumStr.begin(), checksumStr.end(), header.begin() + 148);
        header[154] = '\0';
        header[155] = ' ';

        // Write the tar file.
        wil::unique_hfile file(CreateFileW(TarPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        THROW_LAST_ERROR_IF(!file);

        DWORD written = 0;

        // Write header.
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), header.data(), static_cast<DWORD>(header.size()), &written, nullptr));

        // Write file content.
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), fileContent.data(), static_cast<DWORD>(fileContent.size()), &written, nullptr));

        // Pad to 512-byte boundary.
        auto padding = 512 - (fileContent.size() % 512);
        if (padding < 512)
        {
            std::vector<char> pad(padding, '\0');
            THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), pad.data(), static_cast<DWORD>(pad.size()), &written, nullptr));
        }

        // End-of-archive: two 512-byte blocks of zeros.
        std::array<char, 1024> endOfArchive{};
        THROW_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), endOfArchive.data(), static_cast<DWORD>(endOfArchive.size()), &written, nullptr));
    }
};
} // namespace WSLCE2ETests
